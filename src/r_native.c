// doomgeneric PS2 -- native GS/VU1 pipeline (the hardware half of the renderer).
//
// This file owns everything that needs PS2SDK's libdraw/libgraph/libpacket2/
// libdma headers -- GS + Z-buffer bring-up, the custom VU1 microprogram
// (draw_3D.vsm), texture VRAM management, and per-frame DMA of wall triangles. It
// includes NO Doom headers (PS2SDK's draw.h defines vertex_t/line_t, which clash
// with Doom's r_defs.h). The Doom side (r_gl.c) feeds it geometry + textures
// through the float/buffer-only RN_* API below.
//
// The VU1 microprogram does transform + clip + perspective divide; the GS does
// occlusion with a real Z-buffer. The VU input double-buffer is 496 qw, so walls
// are DMA'd in chunks of CHUNK_VERTS.

#include <kernel.h>
#include <malloc.h>
#include <string.h>

#include <tamtypes.h>
#include <dma.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <graph.h>
#include <draw.h>
#include <gs_psm.h>
#include <math3d.h>

extern u32 VU1Draw3D_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Draw3D_CodeEnd   __attribute__((section(".vudata")));

#define SCR_W   640
#define SCR_H   512
#define TEX_DIM 64                     // per-wall texture in VRAM (64x64x4 = 16KB each)
#define CHUNK_VERTS 216                // 36 walls/chunk; header+verts+sts < 496 qw
#define RN_MAXTEX 384                  // VRAM texture slot table (VRAM itself is the real cap)

static framebuffer_t fb;
static zbuffer_t     zb;
static texbuffer_t   texb;             // "current" texture; .address swapped per bind
static texbuffer_t   tex2d;            // 320x200 UI framebuffer (title/menu/intermission)
static packet2_t*    geom_pkt[2];
static packet2_t*    hdr_pkt;
static int           ctx;
static prim_t        prim;
static lod_t         lod;
static clutbuffer_t  clut;

static MATRIX s_proj  __attribute__((aligned(64)));
static MATRIX s_local __attribute__((aligned(64)));

static VECTOR vpos[CHUNK_VERTS] __attribute__((aligned(64)));
static VECTOR vst [CHUNK_VERTS] __attribute__((aligned(64)));
static int    vcount;

static int rn_tex_addr[RN_MAXTEX];     // VRAM word-addresses of uploaded textures
static int rn_tex_n;

// ---------------------------------------------------------------------------
static void vu1_upload_micro_program(void)
{
    u32 sz = packet2_utils_get_packet_size_for_program(&VU1Draw3D_CodeStart,
                                                       &VU1Draw3D_CodeEnd) + 1;
    packet2_t* p = packet2_create(sz, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    packet2_vif_add_micro_program(p, 0, &VU1Draw3D_CodeStart, &VU1Draw3D_CodeEnd);
    packet2_utils_vu_add_end_tag(p);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(p);
}

static void vu1_set_double_buffer(void)
{
    packet2_t* p = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    packet2_utils_vu_add_double_buffer(p, 8, 496);
    packet2_utils_vu_add_end_tag(p);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(p);
}

static void init_gs(void)
{
    fb.width   = SCR_W;
    fb.height  = SCR_H;
    fb.mask    = 0;
    fb.psm     = GS_PSM_32;
    fb.address = graph_vram_allocate(fb.width, fb.height, fb.psm, GRAPH_ALIGN_PAGE);

    zb.enable  = DRAW_ENABLE;
    zb.mask    = 0;
    zb.method  = ZTEST_METHOD_GREATER_EQUAL;
    zb.zsm     = GS_ZBUF_32;
    zb.address = graph_vram_allocate(fb.width, fb.height, zb.zsm, GRAPH_ALIGN_PAGE);

    texb.width = TEX_DIM;
    texb.psm   = GS_PSM_32;

    graph_initialize(fb.address, fb.width, fb.height, fb.psm, 0, 0);
}

static void init_draw_env(void)
{
    packet2_t* p = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
    packet2_update(p, draw_setup_environment(p->next, 0, &fb, &zb));
    packet2_update(p, draw_primitive_xyoffset(p->next, 0, (2048 - SCR_W/2), (2048 - SCR_H/2)));
    packet2_update(p, draw_finish(p->next));
    dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
    dma_wait_fast();
    packet2_free(p);
}

static void set_prim_lod_clut(void)
{
    lod.calculation = LOD_USE_K;
    lod.max_level   = 0;
    lod.mag_filter  = LOD_MAG_NEAREST;
    lod.min_filter  = LOD_MIN_NEAREST;
    lod.l = 0;
    lod.k = 0;

    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.start = 0; clut.psm = 0; clut.load_method = CLUT_NO_LOAD; clut.address = 0;

    prim.type          = PRIM_TRIANGLE;
    prim.shading       = PRIM_SHADE_GOURAUD;
    prim.mapping       = DRAW_ENABLE;
    prim.fogging       = DRAW_DISABLE;
    prim.blending      = DRAW_DISABLE;
    prim.antialiasing  = DRAW_DISABLE;
    prim.mapping_type  = PRIM_MAP_ST;
    prim.colorfix      = PRIM_UNFIXED;

    texb.info.width      = draw_log2(TEX_DIM);
    texb.info.height     = draw_log2(TEX_DIM);
    texb.info.components = TEXTURE_COMPONENTS_RGB;
    texb.info.function   = TEXTURE_FUNCTION_MODULATE;
}

void RN_Init(void)
{
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);

    geom_pkt[0] = packet2_create(32, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    geom_pkt[1] = packet2_create(32, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    hdr_pkt     = packet2_create(16, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);

    vu1_upload_micro_program();
    vu1_set_double_buffer();

    init_gs();
    init_draw_env();
    set_prim_lod_clut();

    // VRAM for the 2D UI framebuffer (320x200 image in a 512x256 PSM32 area).
    tex2d.width   = 512;
    tex2d.psm     = GS_PSM_32;
    tex2d.address = graph_vram_allocate(512, 256, GS_PSM_32, GRAPH_ALIGN_BLOCK);
    tex2d.info.width      = draw_log2(512);
    tex2d.info.height     = draw_log2(256);
    tex2d.info.components  = TEXTURE_COMPONENTS_RGBA;
    tex2d.info.function    = TEXTURE_FUNCTION_DECAL;

    // hfov = 2*atan(right/near). near=8; right tuned wider than 90 deg. 4:3.
    // ~90deg FOV (matches Doom's BSP visibility, for the edge-hole test) + a near
    // plane pulled in to 2 (the VU drops near-crossing tris, so closer near =
    // fewer vanishing walls / floor holes).
    create_view_screen(s_proj, graph_aspect_ratio(), -2.0f, 2.0f, -1.5f, 1.5f,
                       2.0f, 32768.0f);
}

// ---------------------------------------------------------------------------
static void clear_screen(void)
{
    packet2_t* p = packet2_create(35, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
    packet2_update(p, draw_disable_tests(p->next, 0, &zb));
    packet2_update(p, draw_clear(p->next, 0, 2048.0f - SCR_W/2, 2048.0f - SCR_H/2,
                                 fb.width, fb.height, 0x10, 0x10, 0x18));
    packet2_update(p, draw_enable_tests(p->next, 0, &zb));
    packet2_update(p, draw_finish(p->next));
    dma_wait_fast();
    dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
    packet2_free(p);
    draw_wait_finish();
}

static int rn_r = 128, rn_g = 128, rn_b = 128;   // current modulate colour (light)

static void build_header(int n)
{
    packet2_reset(hdr_pkt, 0);
    packet2_add_float(hdr_pkt, 2048.0f);
    packet2_add_float(hdr_pkt, -2048.0f);                  // negate: GS screen-Y is down, world-Y is up
    packet2_add_float(hdr_pkt, ((float)0xFFFFFF) / 32.0f);
    packet2_add_s32(hdr_pkt, n);
    packet2_utils_gif_add_set(hdr_pkt, 1);
    packet2_utils_gs_add_lod(hdr_pkt, &lod);
    packet2_utils_gs_add_texbuff_clut(hdr_pkt, &texb, &clut);
    packet2_utils_gs_add_prim_giftag(hdr_pkt, &prim, n, DRAW_STQ2_REGLIST, 3, 0);
    packet2_add_u32(hdr_pkt, rn_r);
    packet2_add_u32(hdr_pkt, rn_g);
    packet2_add_u32(hdr_pkt, rn_b);
    packet2_add_u32(hdr_pkt, 128);
}

static void flush_chunk(void)
{
    packet2_t* p;
    u32 hqw;

    if (vcount < 3) { vcount = 0; return; }

    build_header(vcount);
    hqw = packet2_get_qw_count(hdr_pkt);

    p = geom_pkt[ctx];
    packet2_reset(p, 0);
    packet2_utils_vu_add_unpack_data(p, 0, s_local, 8, 0);
    packet2_utils_vu_add_unpack_data(p, 0, hdr_pkt->base, hqw, 1);
    packet2_utils_vu_add_unpack_data(p, hqw, vpos, vcount, 1);
    packet2_utils_vu_add_unpack_data(p, hqw + vcount, vst, vcount, 1);
    packet2_utils_vu_add_start_program(p, 0);
    packet2_utils_vu_add_end_tag(p);

    FlushCache(0);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    ctx = !ctx;
    vcount = 0;
}

// --- texture VRAM cache ----------------------------------------------------
// Create a 32x32 PSM32 slot in VRAM, return its handle (or -1 if VRAM is full).
int RN_TexCreate(void)
{
    int a;
    if (rn_tex_n >= RN_MAXTEX)
        return -1;
    a = graph_vram_allocate(TEX_DIM, TEX_DIM, GS_PSM_32, GRAPH_ALIGN_BLOCK);
    if (a <= 0)
        return -1;
    rn_tex_addr[rn_tex_n] = a;
    return rn_tex_n++;
}

void RN_TexUpload(int h, const void* rgba)
{
    packet2_t* p;
    if (h < 0 || h >= rn_tex_n)
        return;
    FlushCache(0);
    p = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
    packet2_update(p, draw_texture_transfer(p->next, (void*)rgba, TEX_DIM, TEX_DIM,
                                            GS_PSM_32, rn_tex_addr[h], TEX_DIM));
    packet2_update(p, draw_texture_flush(p->next));
    dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
    dma_wait_fast();
    packet2_free(p);
}

// Bind a texture for subsequent walls (flushes the previous texture's batch).
void RN_TexBind(int h)
{
    flush_chunk();
    if (h >= 0 && h < rn_tex_n)
        texb.address = rn_tex_addr[h];
}

// --- float-only geometry API -----------------------------------------------
void RN_FrameBegin(float camx, float camy, float camz, float yaw)
{
    MATRIX m_world, m_view;
    VECTOR cam_pos, cam_rot, zero;

    cam_pos[0]=camx; cam_pos[1]=camy; cam_pos[2]=camz; cam_pos[3]=1.0f;
    cam_rot[0]=0.0f; cam_rot[1]=yaw;  cam_rot[2]=0.0f; cam_rot[3]=1.0f;
    zero[0]=zero[1]=zero[2]=0.0f; zero[3]=1.0f;

    create_local_world(m_world, zero, zero);
    create_world_view(m_view, cam_pos, cam_rot);
    create_local_screen(s_local, m_world, m_view, s_proj);

    clear_screen();
    vcount = 0;
}

// Sector light (0..128) applied as the modulate colour for following walls.
void RN_SetLight(int l)
{
    if (l < 24) l = 24;
    if (l > 128) l = 128;
    if (l != rn_r) {            // light change -> flush so the new colour applies
        flush_chunk();
        rn_r = rn_g = rn_b = l;
    }
}

void RN_AddWall(float x1, float z1, float x2, float z2,
                float yb, float yt, float u0, float u1, float v)
{
    if (yt <= yb)
        return;
    if (vcount + 6 > CHUNK_VERTS)
        flush_chunk();

    #define V(px,py,pz,su,sv) do { \
        vpos[vcount][0]=px; vpos[vcount][1]=py; vpos[vcount][2]=pz; vpos[vcount][3]=1.0f; \
        vst[vcount][0]=su; vst[vcount][1]=sv; vst[vcount][2]=1.0f; vst[vcount][3]=0.0f; \
        vcount++; } while (0)
    V(x1, yb, z1, u0, v);
    V(x2, yb, z2, u1, v);
    V(x2, yt, z2, u1, 0.0f);
    V(x1, yb, z1, u0, v);
    V(x2, yt, z2, u1, 0.0f);
    V(x1, yt, z1, u0, 0.0f);
    #undef V
}

// One arbitrary textured triangle (used for floor/ceiling flats).
void RN_AddTri(float ax, float ay, float az, float as, float at,
               float bx, float by, float bz, float bs, float bt,
               float cx, float cy, float cz, float cs, float ct)
{
    if (vcount + 3 > CHUNK_VERTS)
        flush_chunk();
    #define PV(px,py,pz,su,sv) do { \
        vpos[vcount][0]=px; vpos[vcount][1]=py; vpos[vcount][2]=pz; vpos[vcount][3]=1.0f; \
        vst[vcount][0]=su; vst[vcount][1]=sv; vst[vcount][2]=1.0f; vst[vcount][3]=0.0f; \
        vcount++; } while (0)
    PV(ax,ay,az,as,at);
    PV(bx,by,bz,bs,bt);
    PV(cx,cy,cz,cs,ct);
    #undef PV
}

// Blit a w*h RGBA image as a fullscreen 2D sprite (Z-test off, drawn on top).
// Used for the title / menu / intermission screens (Doom's 2D framebuffer).
void RN_Draw2D(const void* rgba, int w, int h)
{
    packet2_t* p;
    qword_t*   q;
    texrect_t  r;

    FlushCache(0);
    p = packet2_create(48, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
    packet2_update(p, draw_texture_transfer(p->next, (void*)rgba, w, h,
                                            GS_PSM_32, tex2d.address, tex2d.width));
    packet2_update(p, draw_texture_flush(p->next));
    dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
    dma_wait_fast();
    packet2_free(p);

    p = packet2_create(48, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
    q = p->next;
    q = draw_disable_tests(q, 0, &zb);
    q = draw_texturebuffer(q, 0, &tex2d, &clut);
    // draw_rect_textured centres coords at the GS origin (2048), so screen-fill
    // spans -W/2..W/2, -H/2..H/2.
    r.v0.x = -(float)(SCR_W / 2);  r.v0.y = -(float)(SCR_H / 2);  r.v0.z = 0;
    r.t0.u = 0.0f;                 r.t0.v = 0.0f;
    r.v1.x =  (float)(SCR_W / 2);  r.v1.y =  (float)(SCR_H / 2);  r.v1.z = 0;
    r.t1.u = (float)w;             r.t1.v = (float)h;
    r.color.r = r.color.g = r.color.b = r.color.a = 128; r.color.q = 1.0f;
    q = draw_rect_textured(q, 0, &r);
    q = draw_enable_tests(q, 0, &zb);
    q = draw_finish(q);
    packet2_update(p, q);
    dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
    dma_wait_fast();
    packet2_free(p);
    draw_wait_finish();
}

void RN_FrameEnd(void)
{
    flush_chunk();
    graph_wait_vsync();
}
