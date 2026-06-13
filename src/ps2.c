
#include "ps2.h"
#include "pad.h"

/* fjtrujy's ps2_drivers (bundled in ps2sdk/ports) handles loading the correct
   modern IOP modules and binding their EE-side RPC. We use it for cdfs (read
   id1/ from the boot disc) and the USB keyboard/mouse, instead of shipping our
   own stale IRX. init_keyboard_driver/init_mouse_driver pull in usbd and call
   PS2KbdInit/PS2MouseInit themselves, so IN_Init must not init them again. */
#include <ps2_cdfs_driver.h>
#include <ps2_keyboard_driver.h>
#include <ps2_mouse_driver.h>

#include "ps2_settings.h"

/* Whether the USB keyboard/mouse came up -- read by in_ps2.c so the per-frame
   input RPCs are skipped (rather than blocking) when a device isn't present. */
int ps2_kbd_ok = 0;
int ps2_mouse_ok = 0;


void IOP_reset() // resets IOP and apply sbv patches.	
{	
	SifInitRpc(0);
	
	while(!SifIopReset("rom0:UDNL rom0:EELOADCNF",0));
	while(!SifIopSync());
	fioExit();
	SifExitIopHeap();
	SifLoadFileExit();
	SifExitRpc();
	SifExitCmd();
	SifInitRpc(0);
  	FlushCache(0);
  	FlushCache(2);
  	//twice, some in-hdloader hack
  	while(!SifIopReset("rom0:UDNL rom0:EELOADCNF",0));
  	while(!SifIopSync());
  	fioExit();
  	SifExitIopHeap();
  	SifLoadFileExit();
  	SifExitRpc();
  	SifExitCmd();

  	SifInitRpc(0);
  	FlushCache(0);
  	FlushCache(2);

  	SifLoadFileInit();

}	

void loadmodules()
{
   int ret;

   /* Pad path: classic rom modules drive libpad (see Setup_Pad). */
   ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
   if(ret < 0)
          printf("Error loading rom0:SIO2MAN\n");

   ret = SifLoadModule("rom0:PADMAN", 0, NULL);
   if(ret < 0)
          printf("Error loading rom0:PADMAN\n");

   /* Disc filesystem: registers "cdfs:" so COM_InitFilesystem can read id1/. */
   if (init_cdfs_driver() != CDFS_INIT_STATUS_IRX_OK)
          printf("Error loading cdfs driver\n");

   /* USB keyboard/mouse (true => also bring up the usbd dependency). These
      embed the matching modern IRX and call PS2KbdInit/PS2MouseInit. */
   ps2_kbd_ok   = (init_keyboard_driver(true) == KEYBOARD_INIT_STATUS_OK);
   if(!ps2_kbd_ok)
          printf("PS2 keyboard not available\n");

   ps2_mouse_ok = (init_mouse_driver(true) == MOUSE_INIT_STATUS_OK);
   if(!ps2_mouse_ok)
          printf("PS2 mouse not available\n");

   /* Memory card: bring up libmc and load PS2-specific settings (fov, stick
      deadzone, southpaw, ...). Falls back to defaults when no card is present;
      the launcher may already have written the file this boot. */
   PS2Settings_McInit();
   if (PS2Settings_Load())
          printf("ps2quake: settings loaded from memory card\n");
   else
          printf("ps2quake: using default settings (no card / no file)\n");
}
