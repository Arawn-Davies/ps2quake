/*
	cd_ps2.c -- "CD audio" for the PS2 port: music as IMA ADPCM WAV streamed
	from the boot disc and decoded on the EE (a few ops per sample, ~10x
	cheaper than Vorbis and no library / heap allocation), then fed to the
	SPU2 PCM streaming buffer via audsrv. Quake calls CDAudio_Play(track) on
	map change; we stream cdfs:/id1/music/track%02d.wav.

	Tracks are 22050 Hz stereo (the output rate) so no resampling is needed.
	All disc I/O goes through Sys_DiscLock (shared with the game's reads).
*/

#include <kernel.h>
#include <delaythread.h>
#include <fileio.h>
#include <string.h>
#include <stdio.h>

#include "quakedef.h"

extern void Sys_DiscLock (void);
extern void Sys_DiscUnlock (void);
extern cvar_t bgmvolume;

#define OUT_RATE	22050			// must match snd_ps2.c SND_SPEED
#define RING_FRAMES	(OUT_RATE * 4)	// ~4s of slack to ride disc stalls

static short		music_ring[RING_FRAMES * 2];
static volatile int	ring_w = 0, ring_r = 0;

static int			cd_initialized = 0;
static volatile int	thread_run = 0;
static volatile int	playing = 0;
static volatile int	paused = 0;
static volatile int	looping = 0;
static volatile int	want_track = 0;
static int			cur_track = 0;
static int			music_tid = -1;
static char			music_stack[16 * 1024] __attribute__((aligned(16)));

// --- WAV/IMA stream state (music thread only) ---
static int			wav_fd = -1;
static int			wav_channels = 2;
static int			wav_block_align = 1024;
static long			wav_pos = 0;		// current read offset in file
static long			wav_data_start = 0;	// first byte of the data chunk
static long			wav_data_end = 0;	// one past last data byte

// --- resample state (src_step==1 for our 22050 tracks => pass-through) ---
static int			src_rate = OUT_RATE;
static double		src_step = 1.0;
static double		src_frac = 0.0;
static short			src_buf[8192];		// decoded interleaved L,R
static int			src_frames = 0;
static int			src_pos = 0;

// IMA ADPCM tables
static const int ima_index_tab[16] =
	{ -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step_tab[89] = {
	7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
	88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
	544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
	2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
	10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };

// ---- ring buffer (single producer = music thread, consumer = mixer) --------

static int ring_free (void)
{
	int used = ring_w - ring_r;
	if (used < 0)
		used += RING_FRAMES;
	return RING_FRAMES - 1 - used;
}

static void ring_put (short l, short r)
{
	music_ring[ring_w * 2]     = l;
	music_ring[ring_w * 2 + 1] = r;
	ring_w = (ring_w + 1) % RING_FRAMES;
}

// ---- little-endian helpers + locked disc read ------------------------------

static unsigned rd_u16 (const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd_u32 (const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// Read n bytes at file offset `off`; returns bytes read. Seeks every time so
// interleaved game reads on other fds can't disturb our position on cdfs.
static int CD_ReadAt (long off, unsigned char *dst, int n)
{
	int got;

	Sys_DiscLock();
	fioLseek(wav_fd, off, SEEK_SET);
	got = fioRead(wav_fd, dst, n);
	Sys_DiscUnlock();
	return got;
}

// ---- WAV header parse + IMA block decode -----------------------------------

static int CD_ParseWav (void)
{
	unsigned char	buf[40];
	long			pos;

	if (CD_ReadAt(0, buf, 12) < 12)
		return 0;
	if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return 0;

	for (pos = 12; ; )
	{
		unsigned sz;

		if (CD_ReadAt(pos, buf, 8) < 8)
			return 0;
		sz = rd_u32(buf + 4);

		if (!memcmp(buf, "fmt ", 4))
		{
			int fn = (sz > 32) ? 32 : (int)sz;
			if (CD_ReadAt(pos + 8, buf, fn) < fn)
				return 0;
			wav_channels    = rd_u16(buf + 2);
			src_rate        = rd_u32(buf + 4);
			wav_block_align = rd_u16(buf + 12);
		}
		else if (!memcmp(buf, "data", 4))
		{
			wav_data_start = pos + 8;
			wav_data_end   = pos + 8 + sz;
			wav_pos        = wav_data_start;
			return 1;
		}

		pos += 8 + sz + (sz & 1);	// next chunk (word-aligned)
	}
}

// Decode 4 ADPCM bytes (8 nibbles, low then high) -> 8 samples.
static void CD_DecNibbles (const unsigned char *b, int *pred, int *idx, short *out)
{
	int p = *pred, ix = *idx, k;

	for (k = 0; k < 4; k++)
	{
		int byte = b[k];
		int half;

		for (half = 0; half < 2; half++)
		{
			int nib = half ? (byte >> 4) : (byte & 0xf);
			int step = ima_step_tab[ix];
			int diff = step >> 3;

			if (nib & 4) diff += step;
			if (nib & 2) diff += step >> 1;
			if (nib & 1) diff += step >> 2;
			if (nib & 8) p -= diff; else p += diff;
			if (p >  32767) p =  32767; else if (p < -32768) p = -32768;

			ix += ima_index_tab[nib];
			if (ix < 0) ix = 0; else if (ix > 88) ix = 88;

			out[k * 2 + half] = (short)p;
		}
	}
	*pred = p;
	*idx = ix;
}

// Decode one IMA ADPCM WAV block into src_buf; returns stereo frames.
static int CD_DecodeBlock (const unsigned char *blk, int blen)
{
	if (wav_channels == 2)
	{
		int predL = (short)rd_u16(blk + 0), idxL = blk[2];
		int predR = (short)rd_u16(blk + 4), idxR = blk[6];
		const unsigned char *d = blk + 8;
		int groups = (blen - 8) / 8;		// 4 bytes L + 4 bytes R per group
		int fi = 1, g, i;

		if (idxL > 88) idxL = 88;
		if (idxR > 88) idxR = 88;
		src_buf[0] = (short)predL;
		src_buf[1] = (short)predR;

		for (g = 0; g < groups; g++)
		{
			short Ls[8], Rs[8];
			CD_DecNibbles(d + g * 8,     &predL, &idxL, Ls);
			CD_DecNibbles(d + g * 8 + 4, &predR, &idxR, Rs);
			for (i = 0; i < 8; i++)
			{
				src_buf[fi * 2]     = Ls[i];
				src_buf[fi * 2 + 1] = Rs[i];
				fi++;
			}
		}
		return fi;
	}
	else
	{	// mono: 1 header + 8 samples per 4 bytes, duplicated to both channels
		int pred = (short)rd_u16(blk + 0), idx = blk[2];
		const unsigned char *d = blk + 4;
		int groups = (blen - 4) / 4;
		int fi = 1, g, i;

		if (idx > 88) idx = 88;
		src_buf[0] = src_buf[1] = (short)pred;

		for (g = 0; g < groups; g++)
		{
			short s[8];
			CD_DecNibbles(d + g * 4, &pred, &idx, s);
			for (i = 0; i < 8; i++)
			{
				src_buf[fi * 2] = src_buf[fi * 2 + 1] = s[i];
				fi++;
			}
		}
		return fi;
	}
}

// Big read-ahead buffer: one locked disc read per ~3s of audio instead of one
// per 46ms block, so the game's CD activity rarely starves the music stream.
static unsigned char rdbuf[64 * 1024] __attribute__((aligned(64)));
static int rdbuf_len = 0;	// valid bytes
static int rdbuf_off = 0;	// consumed bytes

static void CD_ResetReadAhead (void)
{
	rdbuf_len = 0;
	rdbuf_off = 0;
}

// Decode the next block (from the read-ahead buffer, refilling as needed).
// Returns frames (0 = EOF).
static int CD_DecodeSrc (void)
{
	int blk;

	if (rdbuf_off + wav_block_align > rdbuf_len)
	{
		int want, avail, got;

		avail = (int)(wav_data_end - wav_pos);
		if (avail <= 8)
			return 0;						// end of stream
		want = (int)((sizeof(rdbuf) / wav_block_align) * wav_block_align);
		if (want > avail)
			want = avail;
		got = CD_ReadAt(wav_pos, rdbuf, want);	// one big sequential read
		if (got <= 0)
			return 0;
		wav_pos += got;
		rdbuf_len = got;
		rdbuf_off = 0;
	}

	blk = wav_block_align;
	if (rdbuf_off + blk > rdbuf_len)
		blk = rdbuf_len - rdbuf_off;		// final partial block
	if (blk <= 8)
		return 0;

	src_frames = CD_DecodeBlock(rdbuf + rdbuf_off, blk);
	rdbuf_off += blk;
	src_pos = 0;
	return src_frames;
}

// Produce one output frame at OUT_RATE (box-filter resample; pass-through at
// src_step==1). 0 on EOF.
static int CD_GetFrame (short *l, short *r)
{
	long	suml = 0, sumr = 0;
	int		cnt = 0;

	src_frac += src_step;
	while (src_frac >= 1.0)
	{
		if (src_pos >= src_frames)
			if (CD_DecodeSrc() <= 0)
				break;
		if (src_pos < src_frames)
		{
			suml += src_buf[src_pos * 2];
			sumr += src_buf[src_pos * 2 + 1];
			cnt++;
			src_pos++;
		}
		src_frac -= 1.0;
	}

	if (cnt == 0)
	{
		while (src_pos >= src_frames)
			if (CD_DecodeSrc() <= 0)
				return 0;
		*l = src_buf[src_pos * 2];
		*r = src_buf[src_pos * 2 + 1];
		return 1;
	}

	*l = (short)(suml / cnt);
	*r = (short)(sumr / cnt);
	return 1;
}

static void CD_CloseFile (void)
{
	if (wav_fd >= 0)
	{
		Sys_DiscLock();
		fioClose(wav_fd);
		Sys_DiscUnlock();
		wav_fd = -1;
	}
}

static int CD_OpenTrack (int track)
{
	char path[64];

	CD_CloseFile();
	sprintf(path, "cdfs:/id1/music/track%02d.wav", track);

	Sys_DiscLock();
	wav_fd = fioOpen(path, FIO_O_RDONLY);
	Sys_DiscUnlock();
	if (wav_fd < 0)
	{
		Con_Printf("CDAudio: %s not found\n", path);
		return 0;
	}

	if (!CD_ParseWav())
	{
		Con_Printf("CDAudio: %s is not a usable WAV\n", path);
		CD_CloseFile();
		return 0;
	}

	cur_track = track;
	src_step  = (double)src_rate / (double)OUT_RATE;
	src_frac  = 0.0;
	src_frames = 0;
	src_pos = 0;
	CD_ResetReadAhead();

	Con_Printf("CDAudio: playing track%02d.wav (%d Hz %d ch IMA-ADPCM)\n",
		track, src_rate, wav_channels);
	return 1;
}

static void MusicThread (void *arg)
{
	(void)arg;

	while (thread_run)
	{
		if (want_track >= 2)
		{
			int t = want_track;
			want_track = 0;
			ring_r = ring_w = 0;
			playing = CD_OpenTrack(t) ? 1 : 0;
		}

		if (!playing || paused)
		{
			DelayThread(20000);
			continue;
		}

		{
			int budget = 4096;
			while (playing && budget-- > 0 && ring_free() > 2)
			{
				short l, r;
				if (!CD_GetFrame(&l, &r))
				{
					if (looping)		// rewind to data start
					{
						wav_pos = wav_data_start;
						src_frames = src_pos = 0;
						CD_ResetReadAhead();
						continue;
					}
					playing = 0;
					CD_CloseFile();
					break;
				}
				ring_put(l, r);
			}
		}
		DelayThread(4000);
	}
	ExitThread();
}

// ---- mixer hook: called by the snd_ps2 feed thread -------------------------

void CDAudio_MixStereo16 (short *out, int frames)
{
	float	vol;
	int		i;

	if (!playing || paused)
		return;

	vol = bgmvolume.value;
	if (vol <= 0.0f)
		return;
	if (vol > 1.0f)
		vol = 1.0f;

	for (i = 0; i < frames; i++)
	{
		int l, r;

		if (ring_r == ring_w)
			break;				// underrun: leave the rest as plain SFX

		l = out[i * 2]     + (int)(music_ring[ring_r * 2]     * vol);
		r = out[i * 2 + 1] + (int)(music_ring[ring_r * 2 + 1] * vol);

		if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
		if (r >  32767) r =  32767; else if (r < -32768) r = -32768;

		out[i * 2]     = (short)l;
		out[i * 2 + 1] = (short)r;

		ring_r = (ring_r + 1) % RING_FRAMES;
	}
}

// ---- Quake CDAudio API -----------------------------------------------------

int CDAudio_Init (void)
{
	ee_thread_t	th;
	void		*gp;

	thread_run = 1;

	__asm__ volatile ("move %0, $28" : "=r"(gp));
	memset(&th, 0, sizeof(th));
	th.func             = (void *)MusicThread;
	th.stack            = music_stack;
	th.stack_size       = sizeof(music_stack);
	th.gp_reg           = gp;
	th.initial_priority = 0x30;		// above the game thread, below audio feed,
									// so the ring gets refilled on time
	music_tid = CreateThread(&th);
	if (music_tid < 0)
	{
		Con_Printf("CDAudio_Init: CreateThread failed\n");
		thread_run = 0;
		return -1;
	}
	StartThread(music_tid, NULL);

	cd_initialized = 1;
	Con_Printf("CDAudio: IMA-ADPCM music streamer ready\n");
	return 0;
}

void CDAudio_Play (byte track, qboolean loop)
{
	if (!cd_initialized || track < 2)
		return;

	looping = loop;
	want_track = track;
}

void CDAudio_Stop (void)
{
	if (!cd_initialized)
		return;
	playing = 0;
	want_track = 0;
}

void CDAudio_Pause (void)
{
	paused = 1;
}

void CDAudio_Resume (void)
{
	paused = 0;
}

void CDAudio_Update (void)
{
}

void CDAudio_Shutdown (void)
{
	if (!cd_initialized)
		return;
	thread_run = 0;
	playing = 0;
	if (music_tid >= 0)
	{
		DeleteThread(music_tid);
		music_tid = -1;
	}
	CD_CloseFile();
	cd_initialized = 0;
}
