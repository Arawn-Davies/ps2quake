// ps2_settings.c -- in-RAM settings with optional memory-card persistence.
// See ps2_settings.h. Uses libmc's own file calls (mcOpen/Read/Write/Close),
// which run over their own SIF RPC and so are independent of the cdfs/fio disc
// path the engine serialises. Every call is the async-queue + mcSync(MC_WAIT)
// pair; mcSync's *result holds the real return (fd / byte count / status).

#include <libmc.h>
#include <loadfile.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#include "ps2_settings.h"

#define SAVE_DIR   "PS2QUAKE"
#define SAVE_FILE  "PS2QUAKE/SETTINGS.BIN"

ps2_settings_t ps2_settings;

static int mc_ready = 0;

void PS2Settings_Defaults (ps2_settings_t *s)
{
	memset (s, 0, sizeof (*s));
	s->magic      = PS2CFG_MAGIC;
	s->version    = PS2CFG_VERSION;
	s->size       = sizeof (*s);
	s->renderer   = PS2REND_HARDWARE;   /* default to the GS renderer for now */
	s->video_std  = PS2VID_NTSC_480I;
	s->widescreen = 0;
	s->southpaw   = 0;
	s->deadzone   = 25;   /* matches the old hard-coded 0.25 stick deadzone */
	s->fov        = 90;
	s->render_scale = 1;  /* 1 = full internal res (no GS upscale) */
}

void PS2Settings_McInit (void)
{
	if (mc_ready)
		return;

	/* These are no-ops if already resident (e.g. loaded by sys_ps2). */
	SifLoadModule ("rom0:MCMAN", 0, NULL);
	SifLoadModule ("rom0:MCSERV", 0, NULL);

	if (mcInit (MC_TYPE_MC) >= 0)
		mc_ready = 1;
}

// First port (0=mc0:, 1=mc1:) holding a usable, formatted card, else -1.
int PS2Settings_CardPort (void)
{
	int port, type, freec, format, ret;

	if (!mc_ready)
		return -1;

	for (port = 0; port < 2; port++)
	{
		mcGetInfo (port, 0, &type, &freec, &format);
		mcSync (MC_WAIT, NULL, &ret);
		/* result: 0 = same card present, -1 = card newly inserted (formatted).
		   Both mean a usable card; <= -2 = error / unformatted / no card. */
		if (ret == 0 || ret == -1)
			return port;
	}
	return -1;
}

int PS2Settings_Load (void)
{
	int port, fd, r;
	ps2_settings_t tmp;

	PS2Settings_Defaults (&ps2_settings);

	port = PS2Settings_CardPort ();
	if (port < 0)
		return 0;

	mcOpen (port, 0, SAVE_FILE, O_RDONLY);
	mcSync (MC_WAIT, NULL, &fd);
	if (fd < 0)
		return 0;

	memset (&tmp, 0, sizeof (tmp));
	mcRead (fd, &tmp, sizeof (tmp));
	mcSync (MC_WAIT, NULL, &r);

	mcClose (fd);
	mcSync (MC_WAIT, NULL, &fd);

	if (r >= (int) sizeof (tmp) &&
	    tmp.magic == PS2CFG_MAGIC && tmp.version == PS2CFG_VERSION)
	{
		ps2_settings = tmp;
		return 1;
	}
	return 0;
}

#define ARGV_TAG "ps2cfg="

void PS2Settings_FormatArgv (char *buf, int buflen)
{
	snprintf (buf, buflen, ARGV_TAG "%d.%d.%d.%d.%d.%d",
	          ps2_settings.video_std, ps2_settings.widescreen,
	          ps2_settings.fov, ps2_settings.deadzone, ps2_settings.southpaw,
	          ps2_settings.render_scale);
}

void PS2Settings_ApplyArgv (int argc, char **argv)
{
	int i, vid, ws, fov, dz, sp, rs;

	for (i = 0; i < argc; i++)
	{
		if (!argv[i] || strncmp (argv[i], ARGV_TAG, sizeof (ARGV_TAG) - 1) != 0)
			continue;

		rs = ps2_settings.render_scale;	// optional (older launchers omit it)
		if (sscanf (argv[i] + sizeof (ARGV_TAG) - 1, "%d.%d.%d.%d.%d.%d",
		            &vid, &ws, &fov, &dz, &sp, &rs) >= 5)
		{
			if (vid < 0 || vid >= PS2VID_COUNT) vid = PS2VID_NTSC_480I;
			if (fov < 70)  fov = 70;
			if (fov > 130) fov = 130;
			if (dz  < 0)   dz  = 0;
			if (dz  > 40)  dz  = 40;
			if (rs  < 1)   rs  = 1;
			if (rs  > 3)   rs  = 3;

			ps2_settings.video_std    = (unsigned char) vid;
			ps2_settings.widescreen   = ws ? 1 : 0;
			ps2_settings.fov          = (unsigned char) fov;
			ps2_settings.deadzone     = (unsigned char) dz;
			ps2_settings.southpaw     = sp ? 1 : 0;
			ps2_settings.render_scale = (unsigned char) rs;
		}
		return;
	}
}

int PS2Settings_Save (void)
{
	int port, fd, r;

	ps2_settings.magic   = PS2CFG_MAGIC;
	ps2_settings.version = PS2CFG_VERSION;
	ps2_settings.size    = sizeof (ps2_settings);

	port = PS2Settings_CardPort ();
	if (port < 0)
		return -1;

	/* Ensure the save folder exists; an "already exists" error is harmless. */
	mcMkDir (port, 0, SAVE_DIR);
	mcSync (MC_WAIT, NULL, &r);

	mcOpen (port, 0, SAVE_FILE, O_WRONLY | O_CREAT | O_TRUNC);
	mcSync (MC_WAIT, NULL, &fd);
	if (fd < 0)
		return -2;

	mcWrite (fd, &ps2_settings, sizeof (ps2_settings));
	mcSync (MC_WAIT, NULL, &r);

	mcClose (fd);
	mcSync (MC_WAIT, NULL, &fd);

	return (r == (int) sizeof (ps2_settings)) ? 0 : -3;
}
