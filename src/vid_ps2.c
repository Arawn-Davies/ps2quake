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
// has to fill BASEWIDTH*BASEHEIGHT pixels. Rendering at half the display res
// (exact 2x, same 1.43:1 aspect) roughly halves rasterization cost -- the GS
// does the upscale for free. Bump these toward 640x448 for sharpness, lower
// for speed.
#define	BASEWIDTH	320
#define	BASEHEIGHT	224

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
	VID_InitGS();
}

void	VID_Shutdown (void)
{
	Con_Printf("VID_Shutdown\n");
}

void	VID_Update (vrect_t *rects)
{
	extern int scr_fullupdate;
	unsigned int *clut;
	int i;

	scr_fullupdate = 0;

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
