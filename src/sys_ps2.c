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
	sys_ps2.c -- PlayStation 2 sys driver
	
	by Nicolas Plourde a.k.a nic067 <nicolasplourde@hotmail.com>
	
	See http://www.ps2dev.org for all your ps2 coding need.
*/

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <fileio.h>
#include <signal.h>
#include <debug.h>
#include <timer.h>

#include "quakedef.h"
#include "errno.h"
#include "ps2.h"
#include "pad.h"


cvar_t  sys_linerefresh = {"sys_linerefresh","0"};// set for entity display

qboolean			isDedicated;

//////////////////////////////////////////////////////////////////////////////////////
// SYSCALLS NECESSARY
//////////////////////////////////////////////////////////////////////////////////////
int H_EnableIntcHandler(int inter)
{
	__asm __volatile__  (
							"addiu  $3, $0, 0x0014  \n"      
							"syscall				\n"   
							"nop					\n"
						);
	return 0;
}
int H_DisableIntcHandler(int inter)
{
	__asm __volatile__  (
							"addiu  $3, $0, 0x0015  \n"      
							"syscall				\n"   
							"nop					\n"
						);
	return 0;
}

// LIST OF ID FOR INTERRUPTS

enum
{
INT_GS,
INT_SBUS,
INT_VBLANK_START,
INT_VBLANK_END,
INT_VIF0,
INT_VIF1,
INT_VU0,
INT_VU1,
INT_IPU,
INT_TIMER0,
INT_TIMER1
};

//////////////////////////////////////////////////////////////////////////////////////
// REGISTERS FOR TIMERS
//////////////////////////////////////////////////////////////////////////////////////

// timer T0
#define T0_COUNT      *((volatile unsigned long*)0x10000000)
#define T0_MODE         *((volatile unsigned long*)0x10000010)
#define T0_COMP         *((volatile unsigned long*)0x10000020)
#define T0_HOLD         *((volatile unsigned long*)0x10000030)

// timer T1
#define T1_COUNT      *((volatile unsigned long*)0x10000800)
#define T1_MODE         *((volatile unsigned long*)0x10000810)
#define T1_COMP         *((volatile unsigned long*)0x10000820)
#define T1_HOLD         *((volatile unsigned long*)0x10000830)

unsigned count_time=0;

//////////////////////////////////////////////////////////////////////////////////////
// interrupt handler
//////////////////////////////////////////////////////////////////////////////////////
int handlerItim(int ca)
{
	count_time+=1; // count in steps of 2 ms

	T0_MODE|=1024; // enable next interrupt
	return -1; // only this handler
}

#define TIME_MS 1.0
#define CLOCK_BUS 147456.0

int id_TIM; // id handler

void start_ps2_timer()
{
	T0_MODE=0; // disable timer
	id_TIM=AddIntcHandler(INT_TIMER0,handlerItim,0); // set handler
	H_EnableIntcHandler(INT_TIMER0); // enable handler

	count_time=0; // counter

	T0_COMP=(unsigned) (TIME_MS/(256.0/CLOCK_BUS)); //  adjust comparator to 2 ms
	T0_COUNT=0; // counter at zero
	T0_MODE=256+128+64+2; // set mode to clock=BUSCLK/256, reset to 0,count and interrupt if comparator equal...
}

void stop_ps2_timer()
{
	T0_MODE=0; // disable timer
	H_DisableIntcHandler(INT_TIMER0); // disable handler
	RemoveIntcHandler(INT_TIMER0,id_TIM); // kill handler
}
//////////////////////////////////////////////////////////////////////////

void LoadModules(void) //not used
{
    int ret;

	ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
	if (ret < 0) {
		printf("Failed to load module: SIO2MAN");
		SleepThread();
	}

	ret = SifLoadModule("rom0:MCMAN", 0, NULL);
	if (ret < 0) {
		printf("Failed to load module: MCMAN");
		SleepThread();
	}

	ret = SifLoadModule("rom0:MCSERV", 0, NULL);
	if (ret < 0) {
		printf("Failed to load module: MCSERV");
		SleepThread();
	}
   	

	ret = SifLoadModule("mass:irx/ps2kbd.irx", 0, NULL);
	if (ret < 0) {
		printf("Failed to load module: PS2KBD");
		//SleepThread();
	}	

	ret = SifLoadModule("mass:irx/ps2mouse.irx", 0, NULL);
	if (ret < 0) {
		printf("Failed to load module: PS2MOUSE");
	//	SleepThread();
	}
}

/*
===============================================================================

FILE IO

===============================================================================
*/

#define MAX_HANDLES 10
int sys_handles[MAX_HANDLES];

// One physical drive + non-reentrant legacy fio: serialise every disc op (game
// file reads here AND the music streamer in cd_ps2.c) through one semaphore so
// two threads never drive the fio RPC at once.
static int disc_sema = -1;

void Sys_DiscLock (void)
{
	if (disc_sema >= 0)
		WaitSema(disc_sema);
}

void Sys_DiscUnlock (void)
{
	if (disc_sema >= 0)
		SignalSema(disc_sema);
}

void inithandle (void)
{
	int i;
	ee_sema_t sema;

	for (i = 1; i < MAX_HANDLES; i++)
		sys_handles[i] = -1;

	sema.init_count = 1;
	sema.max_count  = 1;
	sema.option     = 0;
	disc_sema = CreateSema(&sema);
}

int findhandle (void)
{
	int i;
	
	for (i=1 ; i<MAX_HANDLES ; i++)
	{
		if(sys_handles[i] == -1)
		{
			return i;
		}
	}
	Sys_Error ("out of handles");
	return -1;
}

/*
================
filelength
================
*/
int filelength (int f)
{
	int end;

	Sys_DiscLock();
	end = fioLseek(f, 0, SEEK_END);
	fioLseek(f, 0, SEEK_SET);
	Sys_DiscUnlock();

	return end;
}

int Sys_FileOpenRead (char *path, int *hndl)
{
	int f;
	int i;
	
	i = findhandle ();

	/* Use the legacy FIO flags: newlib's O_RDONLY is 0, but the fio drivers
	   (cdfs.irx, usbhdfsd) expect FIO_O_RDONLY (1) and reject 0. */
	Sys_DiscLock();
	f = fioOpen(path, FIO_O_RDONLY);
	Sys_DiscUnlock();
	if (!f)
	{
		*hndl = -1;
		return -1;
	}
	sys_handles[i] = f;
	*hndl = i;
	
	if(filelength(f) < 0)
		return -1;
		
	return filelength(f);
}

int Sys_FileOpenWrite (char *path)
{
	int    f;
	int             i;
	
	i = findhandle ();

	Sys_DiscLock();
	f = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT);
	Sys_DiscUnlock();
	//FIXME
	//if(!f)
	//{
	//	Sys_Error ("Error opening %s: %s", path,strerror(errno));
	//}
	sys_handles[i] = f;
	
	return i;
}

void Sys_FileClose (int handle)
{
	Sys_DiscLock();
	fioClose(sys_handles[handle]);
	Sys_DiscUnlock();
	sys_handles[handle] = -1;
}

void Sys_FileSeek (int handle, int position)
{
	Sys_DiscLock();
	fioLseek(sys_handles[handle], position, SEEK_SET);
	Sys_DiscUnlock();
}

int Sys_FileRead (int handle, void *dest, int count)
{
	int n;

	Sys_DiscLock();
	n = fioRead(sys_handles[handle], dest, count);
	Sys_DiscUnlock();

	return n;
}

int Sys_FileWrite (int handle, void *data, int count)
{
	int n;

	Sys_DiscLock();
	n = fioWrite(sys_handles[handle], data, count);
	Sys_DiscUnlock();

	return n;
}

int     Sys_FileTime (char *path)
{
	int	f;

	Sys_DiscLock();
	f = fioOpen(path, FIO_O_RDONLY);
	if (f >= 0)
		fioClose(f);
	Sys_DiscUnlock();

	return (f >= 0) ? 1 : -1;
}

void Sys_mkdir (char *path)
{
	Sys_DiscLock();
	fioMkdir(path);
	Sys_DiscUnlock();
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
}


void Sys_Error (char *error, ...)
{
	va_list         argptr;

	printf ("Sys_Error: ");   
	va_start (argptr,error);
	vprintf (error,argptr);
	va_end (argptr);
	printf ("\n");

	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list         argptr;
	
	va_start (argptr,fmt);
	vprintf (fmt,argptr);
	va_end (argptr);
}

void Sys_Quit (void)
{    
    IOP_reset();
    __asm__ __volatile__(
    "	li $3, 0x04;"
    "	syscall;"
    "	nop;" );

}

// Wall clock from the EE COP0 cycle counter (cpu_ticks). The original port
// derived time from a Timer0 INTC interrupt (count_time), but that interrupt
// doesn't fire under PCSX2, so the engine clock froze after the first frame.
// Polling cpu_ticks() always advances; we accumulate 32-bit-wrap-safe deltas
// (the counter wraps about every 14s, far longer than a frame).
#define EE_TICKS_PER_SEC	294912000.0		// EE core clock (~294.912 MHz)

float Sys_FloatTime (void)
{
	static int	inited = 0;
	static u32	last;
	static double	seconds = 0.0;
	u32		now;

	now = cpu_ticks();
	if (!inited) { last = now; inited = 1; }
	seconds += (double)(u32)(now - last) / EE_TICKS_PER_SEC;
	last = now;

	return (float)seconds;
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_Sleep (void)
{
}

void Sys_SendKeyEvents (void)
{
}

void Sys_HighFPPrecision (void)
{
}

void Sys_LowFPPrecision (void)
{
}

//=============================================================================

int main (int argc, char **argv)
{ 
    #ifdef _IOPRESET
    IOP_reset();
    #endif
	static quakeparms_t    parms;
	float  time, oldtime, newtime;
    
	//signal(SIGFPE, SIG_IGN);
	//SifInitRpc(0);
	loadmodules();
	//if (SDL_Init(SDL_INIT_AUDIO) < 0)
      //  Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());
	//LoadModules();
/*
	if(mcInit(MC_TYPE_MC) < 0) 
	{
		printf("Failed to initialise memcard\n");
		SleepThread();
	}
*/
	inithandle();
	
	parms.memsize = 24*1024*1024;
	parms.membase = malloc (parms.memsize);
	parms.basedir = ".";

	COM_InitArgv (argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	printf ("Host_Init\n");
	Host_Init (&parms);
	
	start_ps2_timer();
	
	oldtime = Sys_FloatTime () - 0.1;
    while (1)
    {
// find time spent rendering last frame
        newtime = Sys_FloatTime ();
        time = newtime - oldtime;

		oldtime = newtime;

        // Benchmark probe: average rendered FPS to the log every 2s. We run
        // below the 60Hz vsync cap, so this reflects raw render performance.
        {
            static int   fps_n = 0;
            static float fps_t = 0;
            fps_n++;
            if (newtime - fps_t >= 2.0f)
            {
                printf("FPS: %.1f\n", (float)fps_n / (newtime - fps_t));
                fps_n = 0;
                fps_t = newtime;
            }
        }

        Host_Frame (time);
    }
	stop_ps2_timer();
	
	return 0;
}


