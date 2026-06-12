/*
	cd_ps2.c -- "CD audio" for the PS2 port: OGG Vorbis music streamed from the
	boot disc. Quake calls CDAudio_Play(track) on map change; we open
	id1/music/track%02d.ogg and decode it with libvorbisfile in a low-priority
	thread, nearest-neighbour resampling to the 22050 Hz stereo output rate into
	a PCM ring buffer. The audio feed thread (snd_ps2.c) mixes that ring buffer
	on top of the sound effects -> audsrv.

	The OGGs are large (up to 15 MB) so they cannot be preloaded; they stream
	off cdfs via fio, which is non-reentrant and shares the single drive with
	the game's own reads -- so every disc op goes through the Sys_DiscLock
	semaphore (the vorbisfile callbacks below, and sys_ps2.c's Sys_File*).
*/

#include <kernel.h>
#include <delaythread.h>
#include <fileio.h>
#include <string.h>
#include <stdio.h>
#include <vorbis/vorbisfile.h>

#include "quakedef.h"

extern void Sys_DiscLock (void);
extern void Sys_DiscUnlock (void);

extern cvar_t bgmvolume;

#define OUT_RATE	22050			// must match snd_ps2.c SND_SPEED
#define RING_FRAMES	22050			// ~1s of stereo slack to ride disc stalls

static short		music_ring[RING_FRAMES * 2];
static volatile int	ring_w = 0, ring_r = 0;	// producer / consumer (frames)

static int			cd_initialized = 0;
static volatile int	thread_run = 0;
static volatile int	playing = 0;
static volatile int	paused = 0;
static volatile int	looping = 0;
static volatile int	want_track = 0;			// >=2 requests the thread to open
static int			music_tid = -1;
static char			music_stack[16 * 1024] __attribute__((aligned(16)));

// --- decoder state (music thread only) ---
static OggVorbis_File	vf;
static int			vf_open = 0;
static int			ogg_fd = -1;
static int			src_rate = OUT_RATE;
static int			src_chans = 2;
static double		src_step = 1.0;			// src frames advanced per out frame
static double		src_frac = 0.0;
static short			src_buf[8192];			// decoded source (interleaved)
static int			src_frames = 0;			// frames held in src_buf
static int			src_pos = 0;			// consumed frames

// ---- vorbisfile callbacks: raw fio on the cdfs fd. The disc lock is taken by
//      the CALLER around the whole ov_open/ov_read/ov_clear, so a multi-step
//      vorbis op is one atomic transaction the game can't interleave with. ----

static size_t ogg_cb_read (void *ptr, size_t size, size_t nmemb, void *ds)
{
	int got;

	(void)ds;
	got = fioRead(ogg_fd, ptr, (int)(size * nmemb));
	if (got <= 0)
		return 0;
	return (size_t)got / size;
}

static int ogg_cb_seek (void *ds, ogg_int64_t off, int whence)
{
	int r;

	(void)ds;
	r = fioLseek(ogg_fd, (int)off, whence);
	return (r < 0) ? -1 : 0;
}

static long ogg_cb_tell (void *ds)
{
	(void)ds;
	return fioLseek(ogg_fd, 0, SEEK_CUR);
}

static int ogg_cb_close (void *ds)
{
	(void)ds;
	return 0;	// fd is closed by CD_CloseFile so locking stays in one place
}

static ov_callbacks ogg_cbs = { ogg_cb_read, ogg_cb_seek, ogg_cb_close, ogg_cb_tell };

// ---- ring buffer (single producer = music thread, single consumer = mixer) -

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

// ---- decode + resample (music thread) --------------------------------------

static void CD_CloseFile (void)
{
	Sys_DiscLock();
	if (vf_open)
	{
		ov_clear(&vf);
		vf_open = 0;
	}
	if (ogg_fd >= 0)
	{
		fioClose(ogg_fd);
		ogg_fd = -1;
	}
	Sys_DiscUnlock();
}

// Pull one decoded source frame chunk into src_buf. Returns frames (0 = EOF).
static int CD_DecodeSrc (void)
{
	char	raw[4096];
	int		bs, i, n;
	long	ret;
	short  *s;

	Sys_DiscLock();
	ret = ov_read(&vf, raw, sizeof(raw), 0, 2, 1, &bs);	// 16-bit signed LE
	Sys_DiscUnlock();
	if (ret <= 0)
		return 0;

	s = (short *)raw;
	if (src_chans == 1)
	{
		n = (int)ret / 2;					// mono samples -> duplicate to stereo
		for (i = 0; i < n; i++)
		{
			src_buf[i * 2]     = s[i];
			src_buf[i * 2 + 1] = s[i];
		}
	}
	else
	{
		n = (int)ret / 4;					// interleaved stereo frames
		memcpy(src_buf, raw, (size_t)ret);
	}

	src_frames = n;
	src_pos = 0;
	return n;
}

// Produce one output frame at OUT_RATE (nearest-neighbour). 0 on EOF.
static int CD_GetFrame (short *l, short *r)
{
	while (src_pos >= src_frames)
		if (CD_DecodeSrc() <= 0)
			return 0;

	*l = src_buf[src_pos * 2];
	*r = src_buf[src_pos * 2 + 1];

	src_frac += src_step;
	while (src_frac >= 1.0)
	{
		src_frac -= 1.0;
		src_pos++;
		if (src_pos >= src_frames)
			if (CD_DecodeSrc() <= 0)
				return 0;
	}
	return 1;
}

static int CD_OpenTrack (int track)
{
	char		path[64];
	vorbis_info	*vi;

	int ok;

	CD_CloseFile();

	sprintf(path, "cdfs:/id1/music/track%02d.ogg", track);

	// Hold the lock across open + header parse so the seek/read sequence is
	// not interleaved with the game's disc access (which corrupts it on cdfs).
	Sys_DiscLock();
	ogg_fd = fioOpen(path, FIO_O_RDONLY);
	if (ogg_fd < 0)
	{
		Sys_DiscUnlock();
		Con_Printf("CDAudio: %s not found\n", path);
		return 0;
	}
	ok = (ov_open_callbacks(NULL, &vf, NULL, 0, ogg_cbs) >= 0);
	if (ok)
		vf_open = 1;
	else
	{
		fioClose(ogg_fd);
		ogg_fd = -1;
	}
	Sys_DiscUnlock();

	if (!ok)
	{
		Con_Printf("CDAudio: %s is not a valid Ogg\n", path);
		return 0;
	}

	vi = ov_info(&vf, -1);
	src_rate  = vi ? vi->rate : OUT_RATE;
	src_chans = vi ? vi->channels : 2;
	src_step  = (double)src_rate / (double)OUT_RATE;
	src_frac  = 0.0;
	src_frames = 0;
	src_pos = 0;

	Con_Printf("CDAudio: playing track%02d.ogg (%d Hz %d ch)\n",
		track, src_rate, src_chans);
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
			DelayThread(20000);		// 20 ms idle
			continue;
		}

		// Top the ring up; yield periodically so a big disc read by the game
		// (which we may be blocking on the lock) isn't held off too long.
		{
			int budget = 4096;
			while (playing && budget-- > 0 && ring_free() > 2)
			{
				short l, r;
				if (!CD_GetFrame(&l, &r))
				{
					if (looping)
					{
						Sys_DiscLock();
						ov_pcm_seek(&vf, 0);
						Sys_DiscUnlock();
						src_frames = src_pos = 0;
						continue;
					}
					playing = 0;
					CD_CloseFile();
					break;
				}
				ring_put(l, r);
			}
		}
		DelayThread(4000);			// 4 ms
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
	th.initial_priority = 0x50;		// below game + audio feed
	music_tid = CreateThread(&th);
	if (music_tid < 0)
	{
		Con_Printf("CDAudio_Init: CreateThread failed\n");
		thread_run = 0;
		return -1;
	}
	StartThread(music_tid, NULL);

	cd_initialized = 1;
	Con_Printf("CDAudio: OGG music streamer ready\n");
	return 0;
}

void CDAudio_Play (byte track, qboolean loop)
{
	if (!cd_initialized || track < 2)
		return;

	looping = loop;
	want_track = track;		// the music thread opens it (disc I/O off main)
}

void CDAudio_Stop (void)
{
	if (!cd_initialized)
		return;

	playing = 0;
	want_track = 0;
	// file is closed by the music thread next loop (or here if idle); clearing
	// playing is enough to silence the mix immediately.
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
	// Streaming + looping are handled by the music thread; nothing per-frame.
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
