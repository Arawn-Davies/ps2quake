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
// in_ps2.c -- PS2 input driver (USB keyboard/mouse + DualShock pad).

#include <tamtypes.h>
#include <libkbd.h>
#include <libmouse.h>

#include "quakedef.h"
#include "in_ps2.h"
#include "pad.h"
#include "ps2.h"

PS2KbdRawKey key;
PS2MouseData mouse;

struct padButtonStatus buttonst;
float   mouse_x, mouse_y;
float   old_mouse_x, old_mouse_y;

cvar_t		_windowed_mouse = {"_windowed_mouse","0", true};
cvar_t		m_filter = {"m_filter","0", true};

// Engine globals used for analog look/move.
extern cvar_t	cl_forwardspeed;
extern cvar_t	cl_sidespeed;
extern double	host_frametime;

/* Set by loadmodules() (ps2.c) via ps2_drivers: whether the USB keyboard/mouse
   came up. The pad is always present (libpad), so it is read unconditionally;
   the keyboard/mouse read RPCs would block on a missing device, so they are
   gated on these flags. */
extern int ps2_kbd_ok;
extern int ps2_mouse_ok;

void IN_Init (void)
{
	if(ps2_kbd_ok)
		PS2KbdSetReadmode(PS2KBD_READMODE_RAW);

	if(ps2_mouse_ok)
	{
		PS2MouseSetBoundary(0, 639, 0, 479);
		PS2MouseSetReadMode(PS2MOUSE_READMODE_ABS);
		PS2MouseSetPosition(320, 240);
		PS2MouseSetAccel(2.0);
		PS2MouseSetThres(4);
	}

	Setup_Pad();
	Wait_Pad_Ready();

	mouse_x = 320;
	mouse_y = 240;
}

void IN_Shutdown (void)
{
}

// Read the pad only when it is ready, so we never act on garbage button/analog
// data while the controller is still being detected (ps2oom does the same).
static int IN_PadRead (struct padButtonStatus *b)
{
	int s = padGetState(0, 0);

	if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
		return 0;

	return padRead(0, 0, b);
}

// DualShock buttons -> Quake keys. Stock Quake bindings make these work in
// both menus and gameplay: arrows = move/turn + menu nav, MOUSE1 = +attack,
// SPACE = +jump, SHIFT = +speed, ENTER/ESCAPE = menu, TAB = score.
static void IN_PadButtons (void)
{
	static u32	old_pad = 0;
	u32			cur, chg;

	if (IN_PadRead(&buttonst) == 0)
		return;

	cur = 0xffff ^ buttonst.btns;		// 1 = pressed
	chg = cur ^ old_pad;				// changed this frame

	// Emit press/release only on edges so simultaneous buttons and holds work.
	#define PADKEY(bit,qkey) \
		if (chg & (bit)) Key_Event((qkey), (cur & (bit)) != 0)

	PADKEY(PAD_UP,       K_UPARROW);	// +forward / menu up
	PADKEY(PAD_DOWN,     K_DOWNARROW);	// +back    / menu down
	PADKEY(PAD_LEFT,     K_LEFTARROW);	// +left    / menu left
	PADKEY(PAD_RIGHT,    K_RIGHTARROW);	// +right   / menu right
	PADKEY(PAD_CROSS,    K_ENTER);		// menu confirm
	PADKEY(PAD_TRIANGLE, K_ESCAPE);		// menu back
	PADKEY(PAD_START,    K_ESCAPE);		// open menu
	PADKEY(PAD_SELECT,   K_TAB);		// scoreboard
	PADKEY(PAD_SQUARE,   K_SPACE);		// +jump
	PADKEY(PAD_CIRCLE,   K_SHIFT);		// +speed (run)
	PADKEY(PAD_R1,       K_MOUSE1);		// +attack (fire)
	PADKEY(PAD_R2,       K_MOUSE1);		// +attack (fire)
	PADKEY(PAD_L1,       K_MOUSE2);		// secondary
	PADKEY(PAD_L2,       K_CTRL);		// +attack alt

	#undef PADKEY
	old_pad = cur;
}

void IN_Commands (void)
{
	qboolean isKeyDown = false;

	// --- USB keyboard (only if present) ---
	if (ps2_kbd_ok)
	{
		PS2KbdReadRaw(&key);

		if (key.state == PS2KBD_RAWKEY_DOWN)
			isKeyDown = true;

		switch(key.key)
		{
			case PS2_TAB: Key_Event(K_TAB, isKeyDown); break;
			case PS2_ESCAPE: Key_Event(K_ESCAPE, isKeyDown); break;
			case PS2_ENTER: Key_Event(K_ENTER, isKeyDown); break;
			case PS2_SPACE: Key_Event(K_SPACE, isKeyDown); break;
			case PS2_UPARROW: Key_Event(K_UPARROW, isKeyDown); break;
			case PS2_DOWNARROW: Key_Event(K_DOWNARROW, isKeyDown); break;
			case PS2_LEFTARROW: Key_Event(K_LEFTARROW, isKeyDown); break;
			case PS2_RIGHTARROW: Key_Event(K_RIGHTARROW, isKeyDown); break;
			case PS2_BACKSPACE: Key_Event(K_BACKSPACE, isKeyDown); break;
			case PS2_ALT: Key_Event(K_ALT, isKeyDown); break;
			case PS2_CTRL: Key_Event(K_CTRL, isKeyDown); break;
			case PS2_SHIFT: Key_Event(K_SHIFT, isKeyDown); break;
			case PS2_F1: Key_Event(K_F1, isKeyDown); break;
			case PS2_F1+1: Key_Event(K_F2, isKeyDown); break;
			case PS2_F1+2: Key_Event(K_F3, isKeyDown); break;
			case PS2_F1+3: Key_Event(K_F4, isKeyDown); break;
			case PS2_F1+4: Key_Event(K_F5, isKeyDown); break;
			case PS2_F1+5: Key_Event(K_F6, isKeyDown); break;
			case PS2_F1+6: Key_Event(K_F7, isKeyDown); break;
			case PS2_F1+7: Key_Event(K_F8, isKeyDown); break;
			case PS2_F1+8: Key_Event(K_F9, isKeyDown); break;
			case PS2_F1+9: Key_Event(K_F10, isKeyDown); break;
			case PS2_F1+10: Key_Event(K_F11, isKeyDown); break;
			case PS2_F1+11: Key_Event(K_F12, isKeyDown); break;
			case PS2_INS: Key_Event(K_INS, isKeyDown); break;
			case PS2_DEL: Key_Event(K_DEL, isKeyDown); break;
			case PS2_PGDN: Key_Event(K_PGDN, isKeyDown); break;
			case PS2_PGUP: Key_Event(K_PGUP, isKeyDown); break;
			case PS2_HOME: Key_Event(K_HOME, isKeyDown); break;
			case PS2_END: Key_Event(K_END, isKeyDown); break;
			case PS2_PAUSE: Key_Event(K_PAUSE, isKeyDown); break;
			default: Key_Event(us_keymap[key.key], isKeyDown); break;
		}
	}

	// --- USB mouse (only if present) ---
	if (ps2_mouse_ok)
	{
		PS2MouseRead(&mouse);

		if (mouse.buttons > 0)
		{
			switch(mouse.buttons)
			{
				case 1: Key_Event(K_MOUSE1,1);Key_Event(K_MOUSE1,0);break;
				case 2: Key_Event(K_MOUSE2,1);Key_Event(K_MOUSE2,0);break;
				case 4: Key_Event(K_MOUSE3,1);Key_Event(K_MOUSE3,0);break;
				default:break;
			}
		}

		if (mouse.wheel > 0)
		{
			Key_Event(K_MWHEELUP,1);
			Key_Event(K_MWHEELUP,0);
		}
		else if (mouse.wheel < 0)
		{
			Key_Event(K_MWHEELDOWN,1);
			Key_Event(K_MWHEELDOWN,0);
		}
	}

	// --- DualShock (always) ---
	IN_PadButtons();
}

// 0..255 analog axis (centre ~128) -> -1..1 with a centre deadzone.
static float IN_PadAxis (unsigned char v)
{
	float f = ((int)v - 128) / 127.0f;

	if (f >  1.0f) f =  1.0f;
	if (f < -1.0f) f = -1.0f;
	if (f > -0.25f && f < 0.25f)
		return 0.0f;
	return f;
}

void IN_Move (usercmd_t *cmd)
{
	float	lx, ly, rx, ry;

	if (IN_PadRead(&buttonst) == 0)
		return;

	lx = IN_PadAxis(buttonst.ljoy_h);	// left stick  -> move/strafe
	ly = IN_PadAxis(buttonst.ljoy_v);
	rx = IN_PadAxis(buttonst.rjoy_h);	// right stick -> look
	ry = IN_PadAxis(buttonst.rjoy_v);

	// Left stick: strafe + forward/back (up = forward).
	cmd->sidemove    += cl_sidespeed.value    * lx;
	cmd->forwardmove -= cl_forwardspeed.value * ly;

	// Right stick: rate-based look, scaled by frame time so it's framerate
	// independent. Up = look up, right = turn right.
	if (rx != 0.0f)
		cl.viewangles[YAW] -= rx * 180.0f * sensitivity.value * (float)host_frametime;
	if (ry != 0.0f)
	{
		V_StopPitchDrift();
		cl.viewangles[PITCH] += ry * 120.0f * sensitivity.value * (float)host_frametime;
		if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
	}

	// USB mouse look, if a mouse is connected.
	if (ps2_mouse_ok)
	{
		PS2MouseRead(&mouse);
		mouse_x = (float)(mouse.x - 320) * sensitivity.value;
		mouse_y = (float)(mouse.y - 240) * sensitivity.value;
		PS2MouseSetPosition(320, 240);

		cl.viewangles[YAW] -= m_yaw.value * mouse_x;
		if (in_mlook.state & 1)
		{
			V_StopPitchDrift();
			cl.viewangles[PITCH] += m_pitch.value * mouse_y;
			if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
			if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
		}
		mouse_x = mouse_y = 0.0f;
	}
}
