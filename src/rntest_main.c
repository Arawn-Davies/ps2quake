// M1 foundation test for the GS hardware renderer.
//
// Standalone ELF (NOT linked into quake.elf) that brings up the native
// VU1+DMA+GS pipeline in r_native.c and draws one checkerboard-textured triangle
// each frame. Proves the whole path -- GS + Z-buffer init, VU1 microprogram
// upload (draw_3D.vsm), DMA of geometry, and present -- links and runs before
// any Quake integration. Build: ./build.sh -f Makefile.rntest
//
// Camera convention mirrors the working Doom feeder: RN_FrameBegin yaw=0 looks
// toward -Z, so the test triangle sits at z = -30 (well inside near=2..far).

#include <kernel.h>

// r_native.c -- float/handle-only hardware API (no PS2SDK headers needed here).
extern void RN_Init(void);
extern void RN_FrameBegin(float camx, float camy, float camz, float yaw);
extern void RN_FrameEnd(void);
extern void RN_SetLight(int l);
extern int  RN_TexCreate(void);
extern void RN_TexUpload(int h, const void *rgba);
extern void RN_TexBind(int h);
extern void RN_AddTri(float ax, float ay, float az, float as, float at,
                      float bx, float by, float bz, float bs, float bt,
                      float cx, float cy, float cz, float cs, float ct);

#define TEXD 64

static unsigned int checker[TEXD * TEXD] __attribute__((aligned(64)));

static void build_checker(void)
{
    int x, y;
    for (y = 0; y < TEXD; y++)
        for (x = 0; x < TEXD; x++)
        {
            // 8x8 magenta/cyan checker, alpha 0x80 (=1.0 on the GS).
            int on = ((x >> 3) ^ (y >> 3)) & 1;
            unsigned int r = on ? 0xFF : 0x00;
            unsigned int g = on ? 0x00 : 0xFF;
            unsigned int b = 0xFF;
            checker[y * TEXD + x] = r | (g << 8) | (b << 16) | (0x80u << 24);
        }
}

int main(int argc, char **argv)
{
    int tex;

    (void)argc; (void)argv;

    RN_Init();

    build_checker();
    tex = RN_TexCreate();
    if (tex >= 0)
        RN_TexUpload(tex, checker);

    for (;;)
    {
        RN_FrameBegin(0.0f, 0.0f, 0.0f, 0.0f);   // at origin, looking toward -Z
        RN_TexBind(tex);
        RN_SetLight(128);                         // full bright (1.0 modulate)
        // Pushed back to z=-120 so the whole triangle fits on screen with a
        // clear-blue margin around it (at z=-30 a +-15 tri projects off-screen
        // and fills the viewport). Base ~half screen width, apex above centre.
        RN_AddTri(-30.0f, -22.0f, -120.0f, 0.0f, 1.0f,
                   30.0f, -22.0f, -120.0f, 1.0f, 1.0f,
                    0.0f,  26.0f, -120.0f, 0.5f, 0.0f);
        RN_FrameEnd();                            // flush + vsync
    }

    return 0;
}
