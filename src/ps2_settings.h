// ps2_settings.h -- shared PS2-specific settings for ps2quake.
//
// One small struct held in RAM, optionally persisted to a PS2 memory card
// (mc0: then mc1:). The boot launcher and both game ELFs link this so a choice
// made in the launcher (renderer, video standard, widescreen) and one made
// in-game (fov, stick deadzone, southpaw) share the same on-card file.
//
// The disc is read-only, so nothing persists unless the player explicitly saves
// to a memory card. With no card the settings still work for the session and
// fall back to defaults on the next boot.

#ifndef PS2_SETTINGS_H
#define PS2_SETTINGS_H

#define PS2CFG_MAGIC    0x51325053u   /* 'SP2Q' */
#define PS2CFG_VERSION  2

/* video_std */
enum { PS2VID_AUTO = 0, PS2VID_NTSC = 1, PS2VID_PAL = 2 };
/* renderer */
enum { PS2REND_SOFTWARE = 0, PS2REND_HARDWARE = 1 };

typedef struct
{
	unsigned int   magic;       /* PS2CFG_MAGIC */
	unsigned short version;     /* PS2CFG_VERSION */
	unsigned short size;        /* sizeof(this) -- forward-compat guard */
	unsigned char  renderer;    /* PS2REND_*  (which ELF the launcher boots) */
	unsigned char  video_std;   /* PS2VID_*   (AUTO == NTSC) */
	unsigned char  widescreen;  /* 0 = 4:3, 1 = 16:9 (Hor+ fov widening) */
	unsigned char  southpaw;    /* swap move/look sticks */
	unsigned char  deadzone;    /* analog stick deadzone, percent (0..40) */
	unsigned char  fov;         /* horizontal field of view, degrees (70..130) */
	unsigned char  render_scale;/* 3D internal-res divisor 1..3 (GS upscales) */
	unsigned char  reserved[1];
} ps2_settings_t;

/* The live, in-RAM settings. Always valid after PS2Settings_Load(). */
extern ps2_settings_t ps2_settings;

void PS2Settings_Defaults (ps2_settings_t *s);

/* Bring up libmc (loads MCMAN/MCSERV + mcInit). Call once after SifInitRpc.
   Safe to call again; only the first call does work. */
void PS2Settings_McInit (void);

/* Populate ps2_settings: load from mc0: then mc1:, else defaults.
   Returns 1 if a valid file was loaded, 0 if defaults were applied. */
int  PS2Settings_Load (void);

/* Write ps2_settings to the first present card (mc0: then mc1:).
   Returns 0 on success, <0 if no card / write failed. */
int  PS2Settings_Save (void);

/* Port (0 = mc0:, 1 = mc1:) of the first usable card, or -1 if none. */
int  PS2Settings_CardPort (void);

/* Override live settings from a launcher-supplied argv token
   ("ps2cfg=<video>.<widescreen>.<fov>.<deadzone>.<southpaw>"). This is how the
   boot launcher hands its current toggles to the game without needing a card. */
void PS2Settings_ApplyArgv (int argc, char **argv);

/* Format the current settings into the token above (buf must hold >= 48). */
void PS2Settings_FormatArgv (char *buf, int buflen);

#endif /* PS2_SETTINGS_H */
