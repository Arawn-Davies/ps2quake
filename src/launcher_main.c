// ps2quake boot launcher.
//
// Tiny ELF booted first by SYSTEM.CNF. Draws a libdebug text menu, reads the
// DualShock (libpad), and chain-loads the chosen renderer ELF off the disc with
// LoadExecPS2 -- so one ISO carries both the stable software renderer and the
// experimental GS hardware renderer, selectable at boot.
//
// Settings (renderer / video standard / widescreen) are toggles driven by the
// D-pad and held in ps2_settings (shared with the game). They persist to a PS2
// memory card (mc0:/mc1:); the game re-reads that file on its own boot, so a
// choice here carries into the engine. On Launch we auto-save when a card is
// present so the picks take effect without a manual save step.
//
// Disc layout (see make_iso.sh combo): LAUNCH.ELF (this), QUAKESW.ELF (software),
// QUAKEHW.ELF (hardware). Each Quake ELF resets the IOP itself on start, so this
// launcher only needs SIO2MAN/PADMAN (+ MCMAN/MCSERV for the card).

#include <kernel.h>      // LoadExecPS2 (noreturn), SleepThread, FlushCache
#include <sifrpc.h>      // SifInitRpc
#include <loadfile.h>    // SifLoadModule
#include <debug.h>       // init_scr, scr_clear, scr_printf, scr_setXY
#include <libpad.h>
#include <stdio.h>

#include "ps2_settings.h"

#define MENU_TOP   3

enum { ROW_RENDERER = 0, ROW_VIDEO, ROW_WIDE, ROW_SAVE, ROW_LAUNCH, NROWS };

static const char *elfs[2] = {
    "cdrom0:\\QUAKESW.ELF;1",
    "cdrom0:\\QUAKEHW.ELF;1"
};

static char pad_buf[256] __attribute__((aligned(64)));
static int  save_status = -100;   // -100 = not attempted yet; 0 = ok; <0 = err

static void busy_wait(volatile int n)
{
    while (n-- > 0)
        __asm__ volatile ("nop");
}

static int pad_wait_ready(void)
{
    int tries;
    for (tries = 0; tries < 250; tries++)
    {
        int s = padGetState(0, 0);
        if (s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1)
            return 1;
        busy_wait(2000000);
    }
    return 0;
}

static const char *renderer_str(void)
{
    return (ps2_settings.renderer == PS2REND_HARDWARE)
         ? "GS Hardware  (experimental - world only)"
         : "Software     (stable - the complete game)";
}
static const char *video_str(void)
{
    switch (ps2_settings.video_std)
    {
        case PS2VID_NTSC: return "NTSC 60Hz";
        case PS2VID_PAL:  return "PAL 50Hz";
        default:          return "Auto (NTSC)";
    }
}
static const char *wide_str(void)
{
    return ps2_settings.widescreen ? "On  (16:9)" : "Off (4:3)";
}

static void row(int sel, int idx, int y, const char *label, const char *val)
{
    scr_setXY(2, y);
    scr_printf("%s %-12s %s%s%s",
               (idx == sel) ? ">" : " ",
               label,
               val ? "[ " : "", val ? val : "", val ? " ]" : "");
}

static void draw(int sel, int card_port)
{
    scr_clear();
    scr_setXY(2, MENU_TOP);
    scr_printf("ps2quake  --  boot options");

    row(sel, ROW_RENDERER, MENU_TOP + 2, "Renderer:",   renderer_str());
    row(sel, ROW_VIDEO,    MENU_TOP + 3, "Video:",       video_str());
    row(sel, ROW_WIDE,     MENU_TOP + 4, "Widescreen:",  wide_str());

    scr_setXY(2, MENU_TOP + 6);
    scr_printf("%s Save settings to memory card", (sel == ROW_SAVE) ? ">" : " ");
    scr_setXY(2, MENU_TOP + 7);
    scr_printf("%s Launch", (sel == ROW_LAUNCH) ? ">" : " ");

    scr_setXY(2, MENU_TOP + 9);
    if (card_port < 0)
        scr_printf("Memory card: none detected (settings won't persist)");
    else
        scr_printf("Memory card: mc%d:  detected", card_port);

    scr_setXY(2, MENU_TOP + 10);
    if (save_status == 0)
        scr_printf("Saved.");
    else if (save_status > -100)
        scr_printf("Save failed (no card?).");

    scr_setXY(2, MENU_TOP + 12);
    scr_printf("Up/Down: select   Left/Right: change   Cross: confirm");
    scr_setXY(2, MENU_TOP + 13);
    scr_printf("Start: launch now");
}

// Cycle a toggle row's value by +dir (with wraparound).
static void adjust(int sel, int dir)
{
    save_status = -100;   // value changed; clear stale "Saved." line
    switch (sel)
    {
        case ROW_RENDERER:
            ps2_settings.renderer ^= 1;
            break;
        case ROW_VIDEO:
            ps2_settings.video_std = (unsigned char)
                ((ps2_settings.video_std + 3 + dir) % 3);
            break;
        case ROW_WIDE:
            ps2_settings.widescreen ^= 1;
            break;
    }
}

int main(int argc, char **argv)
{
    struct padButtonStatus btn;
    int sel = ROW_LAUNCH, last = -1, last_card = -2;
    int launch = 0;
    u16 prev = 0xFFFF;   // active-low: all released

    (void) argc; (void) argv;

    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    init_scr();
    scr_clear();
    scr_setXY(2, MENU_TOP);
    scr_printf("ps2quake launcher -- initialising...");

    // Memory card + saved settings (so the toggles reflect last choice).
    PS2Settings_McInit();
    PS2Settings_Load();
    sel = ROW_LAUNCH;

    padInit(0);
    padPortOpen(0, 0, pad_buf);

    if (pad_wait_ready())
    {
        while (!launch)
        {
            int card_port = PS2Settings_CardPort();
            if (sel != last || card_port != last_card)
            {
                draw(sel, card_port);
                last = sel;
                last_card = card_port;
            }
            if (padRead(0, 0, &btn) != 0)
            {
                u16 now     = btn.btns;
                u16 pressed = (prev & ~now);   // 1->0 edge: button pressed
                prev = now;

                if (pressed & PAD_UP)    { sel = (sel - 1 + NROWS) % NROWS; }
                if (pressed & PAD_DOWN)  { sel = (sel + 1) % NROWS; }
                if (pressed & PAD_LEFT)  { adjust(sel, -1); last = -1; }
                if (pressed & PAD_RIGHT) { adjust(sel, +1); last = -1; }

                if (pressed & PAD_CROSS)
                {
                    if (sel == ROW_LAUNCH)
                        launch = 1;
                    else if (sel == ROW_SAVE)
                        { save_status = PS2Settings_Save(); last = -1; }
                    else
                        { adjust(sel, +1); last = -1; }
                }
                if (pressed & PAD_START)
                    launch = 1;
            }
            busy_wait(1500000);
        }
    }
    else
    {
        // No controller -> launch the saved/default renderer.
        launch = 1;
    }

    // Persist on the way out so the game ELF reads these choices on its boot.
    if (PS2Settings_CardPort() >= 0)
        PS2Settings_Save();

    scr_clear();
    scr_setXY(2, MENU_TOP);
    scr_printf("Launching %s renderer...",
               (ps2_settings.renderer == PS2REND_HARDWARE) ? "hardware" : "software");

    padPortClose(0, 0);
    padEnd();

    // Hand the live toggles to the game via argv so they apply even without a
    // memory card (the card, if present, persists them across cold boots).
    {
        static char cfg[48];
        char *path = (char *) elfs[ps2_settings.renderer & 1];
        char *args[2];

        PS2Settings_FormatArgv(cfg, sizeof(cfg));
        args[0] = path;
        args[1] = cfg;
        LoadExecPS2(path, 2, args);   // noreturn
    }
    SleepThread();
    return 0;
}
