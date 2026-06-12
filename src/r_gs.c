// r_gs.c -- Quake-side feeder for the native GS hardware renderer (M2b).
//
// Walks the world model's BSP surfaces and emits them as textured triangles to
// the native VU1+DMA+GS pipeline (r_native.c) through the float-only RN_* API.
// Includes Quake's headers (model_t/msurface_t/r_refdef) but NOT PS2SDK's
// draw.h (which clashes), mirroring the doomgeneric r_gl.c / r_native.c split.
//
// M2b scope: draw EVERY world surface every frame with a dev-grid texture, no
// PVS/frustum culling -- the GS hardware Z-buffer does occlusion. This proves
// the BSP->GS feed (geometry + ST + camera). Culling, real textures, lightmaps,
// entities, sky and water are later milestones.

#include "quakedef.h"

// Native pipeline (r_native.c) -- no Doom/PS2SDK type clash.
extern void RN_FrameBegin(float camx, float camy, float camz,
                          float pitch, float yaw, float roll);
extern void RN_FrameEnd(void);
extern void RN_SetLight(int l);
extern int  RN_TexCreate(void);
extern void RN_TexUpload(int h, const void *rgba);
extern void RN_TexBind(int h);
extern void RN_AddTri(float ax, float ay, float az, float as, float at,
                      float bx, float by, float bz, float bs, float bt,
                      float cx, float cy, float cz, float cs, float ct);

// Set when RGS_RenderWorld presented a 3D frame -> VID_Update skips its 2D blit.
int rgs_drew_world = 0;

#define DEG2RAD     (3.14159265358979f / 180.0f)
#define RGS_TEXD    64
#define MAXSURFV    64				// max verts per reconstructed surface polygon

static int          rgs_devtex = -1;
static unsigned int rgs_devpix[RGS_TEXD * RGS_TEXD] __attribute__((aligned(64)));

// Build a neutral grey 16px checker dev texture (so surface orientation/depth
// reads clearly without real textures yet).
void RGS_Init(void)
{
    int x, y;
    for (y = 0; y < RGS_TEXD; ++y)
        for (x = 0; x < RGS_TEXD; ++x)
        {
            int on = ((x >> 4) ^ (y >> 4)) & 1;
            unsigned int c = on ? 0xC0u : 0x60u;
            rgs_devpix[y * RGS_TEXD + x] = c | (c << 8) | (c << 16) | (0x80u << 24);
        }
    rgs_devtex = RN_TexCreate();
    if (rgs_devtex >= 0)
        RN_TexUpload(rgs_devtex, rgs_devpix);
}

// Reconstruct one surface's polygon from its edges and emit it as a triangle fan.
// Quake world is (x, y, z) with z up; the renderer wants (x, z, -y).
static void emit_surface(model_t *m, msurface_t *surf)
{
    float	px[MAXSURFV], py[MAXSURFV], pz[MAXSURFV];
    float	ss[MAXSURFV], tt[MAXSURFV];
    int		n = surf->numedges, i;
    float	*v0 = surf->texinfo->vecs[0];
    float	*v1 = surf->texinfo->vecs[1];

    if (n < 3)
        return;
    if (n > MAXSURFV)
        n = MAXSURFV;

    for (i = 0; i < n; ++i)
    {
        int			li = m->surfedges[surf->firstedge + i];
        mvertex_t	*mv = (li > 0) ? &m->vertexes[m->edges[li].v[0]]
                                   : &m->vertexes[m->edges[-li].v[1]];
        float		*p = mv->position;

        px[i] =  p[0];
        py[i] =  p[2];
        pz[i] = -p[1];
        // ST in texels (texinfo maps world->texel); scaled to tile the 64px
        // dev texture a few times across a surface.
        ss[i] = (p[0]*v0[0] + p[1]*v0[1] + p[2]*v0[2] + v0[3]) * (1.0f/64.0f);
        tt[i] = (p[0]*v1[0] + p[1]*v1[1] + p[2]*v1[2] + v1[3]) * (1.0f/64.0f);
    }

    for (i = 1; i + 1 < n; ++i)
        RN_AddTri(px[0],   py[0],   pz[0],   ss[0],   tt[0],
                  px[i],   py[i],   pz[i],   ss[i],   tt[i],
                  px[i+1], py[i+1], pz[i+1], ss[i+1], tt[i+1]);
}

// Draw the whole world. Camera comes from r_refdef (set by V_RenderView before
// R_RenderView, so it's valid even though we skip the software R_SetupFrame).
void RGS_RenderWorld(void)
{
    model_t	*m = cl.worldmodel;
    float	cx, cy, cz, pitch, yaw, roll;
    int		first, count, i;

    rgs_drew_world = 0;
    if (!m)
        return;

    // Quake view origin/angles -> renderer camera. Angle mapping is a first
    // best-effort (signs/offset likely need a boot iteration or two).
    cx =  r_refdef.vieworg[0];
    cy =  r_refdef.vieworg[2];
    cz = -r_refdef.vieworg[1];
    pitch = -r_refdef.viewangles[0] * DEG2RAD;
    yaw   = -r_refdef.viewangles[1] * DEG2RAD - 1.57079633f;	// -90 deg, as Doom
    roll  =  r_refdef.viewangles[2] * DEG2RAD;

    RN_FrameBegin(cx, cy, cz, pitch, yaw, roll);
    RN_SetLight(128);
    RN_TexBind(rgs_devtex);

    first = m->firstmodelsurface;
    count = m->nummodelsurfaces;
    if (count <= 0) { first = 0; count = m->numsurfaces; }

    for (i = first; i < first + count; ++i)
    {
        msurface_t *surf = &m->surfaces[i];
        if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
            continue;					// sky / liquids handled later
        emit_surface(m, surf);
    }

    RN_FrameEnd();
    rgs_drew_world = 1;
}
