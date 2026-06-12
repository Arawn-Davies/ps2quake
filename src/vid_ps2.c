/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/*
	vid_ps2.c -- PlayStation 2 video driver

	gsKit backend: Quake's software renderer fills an 8-bit indexed buffer
	(vid.buffer) which we hand to the GS as a PSM_T8 texture plus a CT32 CLUT
	(the palette). The GS does the palette expansion AND the scale-to-display
	in hardware, so the EE never expands pixels. This mirrors the working
	doomgeneric PS2 gsKit path (PSMT8 + CLUT). The original hand-rolled GIF
	blit (ps2_gs.c) is no longer used.
*/

#include "quakedef.h"
#include "d_local.h"

#include <gsKit.h>
#include <dmaKit.h>
#include <malloc.h>

viddef_t	vid;				// global video state

static int verbose=0;
int ignorenext;

// Internal software-render resolution. The GS hardware-scales this up to the
// DISPLAY_W x DISPLAY_H output (bilinear), so the EE software rasterizer only
// has to fill BASEWIDTH*BASEHEIGHT pixels. The closer this is to the 640x448
// display, the sharper the image (less GS upscale) but the more pixels the EE
// must rasterize. 384x268 is a ~1.67x upscale (vs 2x at 320x224) for ~1.44x the
// fill cost; width is a multiple of 64 to keep the GS T8 texture buffer aligned.
// Bump toward 640x448 for sharpness, drop toward 320x224 for speed.
#define	BASEWIDTH	384
#define	BASEHEIGHT	268

// GS display size: the BASEWIDTH x BASEHEIGHT software frame is GS-scaled to
// fill this (NTSC 640x448, as in the working doom port -- shows in PCSX2 and
// on any TV; the disc's region is independent of the GS video timing).
#define	DISPLAY_W	640
#define	DISPLAY_H	448

byte	*vid_buffer;
long	zbuffer;
void	*surfcache;
int		surfcachesize;

static long highhunkmark;
static long buffersize;

unsigned short	d_8to16table[256];

// 8-bit palette (0..255 per channel), rebuilt into the GS CLUT each frame.
static unsigned char pal_r[256], pal_g[256], pal_b[256];

static GSGLOBAL	*gsGlobal = NULL;
static GSTEXTURE	tex;			// BASEWIDTH x BASEHEIGHT PSM_T8 + CT32 CLUT

#ifdef R_HARDWARE
// M2a: present through the native VU1+DMA+GS core (r_native.c) instead of gsKit.
// For now the software frame is palette-expanded to RGBA and blitted fullscreen
// via RN_Draw2D -- this retires gsKit and proves the engine runs on the hardware
// backend. M2b replaces the 3D world with real GS geometry; the 2D path here
// becomes just the HUD overlay.
extern void RN_Init(void);
extern void RN_FrameBegin(float, float, float, float, float, float);
extern void RN_FrameEnd(void);
extern void RN_Draw2D(const void *rgba, int w, int h);
extern int  rgs_drew_world;		// r_gs.c -- set when R_RenderView drew the 3D world
#endif

void ResetFrameBuffer(void)
{
	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (highhunkmark);
		d_pzbuffer = NULL;
	}
	highhunkmark = Hunk_HighMark ();

// alloc an extra line in case we want to wrap, and allocate the z-buffer
	buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

	surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

	buffersize += surfcachesize;

	d_pzbuffer = Hunk_HighAllocName (buffersize, "video");
	if (d_pzbuffer == NULL)
		Sys_Error ("Not enough memory for video mode\n");

	surfcache = (byte *) d_pzbuffer + vid.width * vid.height * sizeof (*d_pzbuffer);

	D_InitCaches(surfcache, surfcachesize);

	vid.buffer = vid_buffer;
	vid.conbuffer = vid.buffer;
}

void VID_SetPalette (unsigned char *palette)
{
	int i;
	for (i=0 ; i<256 ; i++)
	{
		pal_r[i] = palette[i*3+0];
		pal_g[i] = palette[i*3+1];
		pal_b[i] = palette[i*3+2];
	}
}

void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette(palette);
}

static void VID_InitGS (void)
{
	gsGlobal = gsKit_init_global();

	gsGlobal->Mode			= GS_MODE_NTSC;
	gsGlobal->Interlace		= GS_INTERLACED;
	gsGlobal->Field			= GS_FIELD;
	gsGlobal->Width			= DISPLAY_W;
	gsGlobal->Height		= DISPLAY_H;
	gsGlobal->PSM			= GS_PSM_CT24;
	gsGlobal->PSMZ			= GS_PSMZ_16S;
	gsGlobal->ZBuffering	= GS_SETTING_OFF;
	gsGlobal->DoubleBuffering = GS_SETTING_ON;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
				D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gsKit_init_screen(gsGlobal);
	gsKit_mode_switch(gsGlobal, GS_ONESHOT);	// queue + flip every frame
	gsKit_TexManager_init(gsGlobal);

	// Software frame uploaded as an 8-bit indexed texture + 256-entry CT32
	// CLUT. Mem is a 128-aligned staging buffer (the GS DMA wants aligned
	// source); we copy vid.buffer into it each frame.
	tex.Width	= vid.width;
	tex.Height	= vid.height;
	tex.PSM		= GS_PSM_T8;
	tex.ClutPSM	= GS_PSM_CT32;
	tex.Filter	= GS_FILTER_LINEAR;
	tex.Delayed	= 0;
	tex.Vram	= 0;
	tex.VramClut = 0;
	tex.Mem		= memalign(128, gsKit_texture_size_ee(tex.Width, tex.Height, tex.PSM));
	tex.Clut	= memalign(128, gsKit_texture_size_ee(256, 1, tex.ClutPSM));
}

void	VID_Init (unsigned char *palette)
{
	ignorenext=0;
	vid.width = BASEWIDTH;
	vid.height = BASEHEIGHT;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.numpages = 2;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	verbose=COM_CheckParm("-verbose");

	vid_buffer = malloc(vid.width*vid.height);

	ResetFrameBuffer();

	vid.rowbytes = vid.width;
	vid.buffer = vid_buffer;
	vid.direct = 0;
	vid.conbuffer = vid_buffer;
	vid.conrowbytes = vid.rowbytes;
	vid.conwidth = vid.width;
	vid.conheight = vid.height;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

	VID_SetPalette(palette);
#ifdef R_HARDWARE
	{
		extern void RGS_Init (void);
		RN_Init();		// native GS/VU1 bring-up (owns the GS instead of gsKit)
		RGS_Init();		// world feeder: dev texture + state
	}
#else
	VID_InitGS();
#endif
}

void	VID_Shutdown (void)
{
	Con_Printf("VID_Shutdown\n");
}

void	VID_Update (vrect_t *rects)
{
	extern int scr_fullupdate;

	scr_fullupdate = 0;

#ifdef R_HARDWARE
	{
		// If R_RenderView already drew + presented the 3D world this frame (M2b),
		// VID_Update has nothing to do -- the HUD overlay comes in M2c. Otherwise
		// (menus / console / loading, no 3D this frame) present the 8-bit software
		// frame palette-expanded to RGBA, fullscreen, through the native GS path.
		static unsigned int rgba[BASEWIDTH * BASEHEIGHT] __attribute__((aligned(128)));
		static unsigned int pal32[256];
		const unsigned char *src = (const unsigned char *) vid.buffer;
		int n = vid.width * vid.height, k;

		(void) rects;
		if (rgs_drew_world)
		{
			rgs_drew_world = 0;
			return;
		}

		for (k = 0; k < 256; ++k)
			pal32[k] = (unsigned int) pal_r[k]
					 | ((unsigned int) pal_g[k] << 8)
					 | ((unsigned int) pal_b[k] << 16)
					 | (0x80u << 24);
		for (k = 0; k < n; ++k)
			rgba[k] = pal32[src[k]];

		RN_FrameBegin(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		RN_Draw2D(rgba, vid.width, vid.height);
		RN_FrameEnd();
		return;
	}
#else
	unsigned int *clut;
	int i;

	// Rebuild the CLUT from the current palette each frame (cheap, 256 entries)
	// so palette effects (damage flash, item pickup, powerups) show. GS RGBA32
	// is R | G<<8 | B<<16 | A<<24; A=0x80 is 1.0 (opaque).
	//
	// A 256-entry CT32 CLUT for a PSM_T8 texture is stored swizzled (CSM1):
	// index bits 3 and 4 are swapped in VRAM. gsKit doesn't correct this, so we
	// write each colour to the swizzled slot (otherwise the image mis-tints).
	clut = (unsigned int *) tex.Clut;
	for (i = 0; i < 256; ++i)
	{
		int j = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
		clut[j] =  (unsigned int) pal_r[i]
				| ((unsigned int) pal_g[i] << 8)
				| ((unsigned int) pal_b[i] << 16)
				| (0x80u << 24);
	}

	// 8-bit indices -> aligned upload buffer.
	memcpy(tex.Mem, vid.buffer, (size_t) vid.width * vid.height);

	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));

	gsKit_TexManager_invalidate(gsGlobal, &tex);	// force re-upload (changed)
	gsKit_TexManager_bind(gsGlobal, &tex);

	gsKit_prim_sprite_texture(gsGlobal, &tex,
		0.0f, 0.0f,									// screen x1,y1
		0.0f, 0.0f,									// tex   u1,v1
		(float) gsGlobal->Width, (float) gsGlobal->Height,	// x2,y2 (fill display)
		(float) vid.width, (float) vid.height,		// u2,v2
		0,											// z
		GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));	// 1.0 modulate

	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
	gsKit_TexManager_nextFrame(gsGlobal);
#endif	// R_HARDWARE
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}
