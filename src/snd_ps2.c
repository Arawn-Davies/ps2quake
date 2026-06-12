/*
	snd_ps2.c -- PlayStation 2 sound output (SNDDMA backend) via audsrv.

	Quake's mixer (snd_dma.c/snd_mix.c) paints 16-bit samples into a ring
	buffer (shm->buffer) ahead of the play position. There is no real DMA here:
	a dedicated EE thread *is* the "hardware" -- it walks shm->buffer linearly
	and pushes chunks to audsrv, which consumes them at the output rate. audsrv
	audio modules are brought up by ps2_drivers (init_audio_driver), mirroring
	the doomgeneric PS2 audsrv path. SNDDMA_GetDMAPos reports how far the feed
	thread has consumed so the mixer knows how far ahead to paint.
*/

#include <audsrv.h>
#include <ps2_audio_driver.h>
#include <kernel.h>
#include <malloc.h>
#include <string.h>

#include "quakedef.h"

extern volatile dma_t	sn;			// defined in snd_dma.c
extern volatile dma_t  *shm;

// Music mixer hook (cd_ps2.c): adds OGG music on top of the SFX chunk.
extern void CDAudio_MixStereo16 (short *out, int frames);

#define SND_SPEED		22050
#define SND_CHANNELS	2
#define SND_BITS		16
#define SND_SAMPLES		32768			// total samples (frames*channels), pow2
#define SND_BYTES		(SND_SAMPLES * (SND_BITS/8))	// ring buffer size
#define FEED_CHUNK		1024			// bytes pushed to audsrv per step

static volatile int		snd_running = 0;
static volatile unsigned	feed_bytes = 0;	// total bytes consumed by feed thread
static int			feed_tid = -1;
static char			feed_stack[16 * 1024] __attribute__((aligned(16)));

// Walk shm->buffer linearly and stream it to audsrv. audsrv_wait_audio blocks
// until the audio ring has room, so this self-paces to the output rate and
// runs in short bursts without starving the game thread.
static void FeedThread(void *arg)
{
	static char	mix[FEED_CHUNK] __attribute__((aligned(16)));
	unsigned	pos = 0;				// byte offset into shm->buffer

	(void)arg;
	while (snd_running)
	{
		audsrv_wait_audio(FEED_CHUNK);

		// Gather one chunk of mixed SFX from the ring (wrap-aware).
		if (pos + FEED_CHUNK <= SND_BYTES)
		{
			memcpy(mix, (char *)shm->buffer + pos, FEED_CHUNK);
			pos += FEED_CHUNK;
		}
		else
		{
			unsigned first = SND_BYTES - pos;	// wrap split
			memcpy(mix, (char *)shm->buffer + pos, first);
			memcpy(mix + first, (char *)shm->buffer, FEED_CHUNK - first);
			pos = FEED_CHUNK - first;
		}
		if (pos >= SND_BYTES)
			pos -= SND_BYTES;

		// Add OGG music on top (16-bit stereo -> FEED_CHUNK/4 frames).
		CDAudio_MixStereo16((short *)mix, FEED_CHUNK / 4);

		audsrv_play_audio(mix, FEED_CHUNK);

		feed_bytes += FEED_CHUNK;
	}
	ExitThread();
}

qboolean SNDDMA_Init(void)
{
	struct audsrv_fmt_t	fmt;
	ee_thread_t		th;
	void			*gp;

	if (init_audio_driver() != AUDIO_INIT_STATUS_OK)
	{
		Con_Printf("SNDDMA_Init: init_audio_driver failed\n");
		return false;
	}

	fmt.bits     = SND_BITS;
	fmt.freq     = SND_SPEED;
	fmt.channels = SND_CHANNELS;
	if (audsrv_set_format(&fmt) != 0)
	{
		Con_Printf("SNDDMA_Init: audsrv_set_format failed\n");
		return false;
	}
	audsrv_set_volume(MAX_VOLUME);

	shm = &sn;
	shm->splitbuffer		= 0;
	shm->samplebits			= SND_BITS;
	shm->speed			= SND_SPEED;
	shm->channels			= SND_CHANNELS;
	shm->samples			= SND_SAMPLES;
	shm->samplepos			= 0;
	shm->soundalive			= true;
	shm->gamealive			= true;
	shm->submission_chunk	= 1;
	shm->buffer = memalign(64, SND_BYTES);
	if (!shm->buffer)
		return false;
	memset((void *)shm->buffer, 0, SND_BYTES);

	feed_bytes = 0;
	snd_running = 1;

	// Run the feed thread above the game thread so audio chunks are pushed the
	// instant audsrv needs them, even during render-heavy frames; it blocks on
	// audsrv_wait_audio, so it yields back immediately.
	ChangeThreadPriority(GetThreadId(), 0x40);

	__asm__ volatile ("move %0, $28" : "=r"(gp));
	memset(&th, 0, sizeof(th));
	th.func             = (void *)FeedThread;
	th.stack            = feed_stack;
	th.stack_size       = sizeof(feed_stack);
	th.gp_reg           = gp;
	th.initial_priority = 0x20;
	feed_tid = CreateThread(&th);
	if (feed_tid < 0)
	{
		snd_running = 0;
		Con_Printf("SNDDMA_Init: CreateThread failed\n");
		return false;
	}
	StartThread(feed_tid, NULL);

	Con_Printf("SNDDMA_Init: audsrv %d Hz %d-bit %d ch\n",
		SND_SPEED, SND_BITS, SND_CHANNELS);
	return true;
}

// Play position in samples (0..shm->samples), wrapping -- used by GetSoundtime.
int SNDDMA_GetDMAPos(void)
{
	shm->samplepos = (feed_bytes / (SND_BITS/8)) % SND_SAMPLES;
	return shm->samplepos;
}

void SNDDMA_Submit(void)
{
	// The feed thread streams continuously; nothing to flush.
}

void SNDDMA_Shutdown(void)
{
	snd_running = 0;
	if (feed_tid >= 0)
	{
		DeleteThread(feed_tid);
		feed_tid = -1;
	}
	if (shm && shm->buffer)
	{
		free((void *)shm->buffer);
		shm->buffer = NULL;
	}
}
