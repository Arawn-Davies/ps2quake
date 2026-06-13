// r_gs.c -- Quake-side feeder for the native GS hardware renderer (M2c/M2d).
//
// Walks the world model's BSP surfaces and emits them as textured triangles to
// the native VU1+DMA+GS pipeline (r_native.c) through the float-only RN_* API.
// Includes Quake's headers (model_t/msurface_t/r_refdef) but NOT PS2SDK's
// draw.h (which clashes), mirroring the doomgeneric r_gl.c / r_native.c split.
//
// M2c: PVS + backface culling (only surfaces in visible leaves that face the
// camera). M2d: each surface polygon is clipped in CLIP SPACE against the +-w
// planes using the renderer's own MVP (RN_TransformToClip), so no triangle
// crosses the near plane or overflows the GS guard band -- the VU only does
// trivial-reject, not geometric clipping, so unclipped large surfaces left
// holes. Clip space is used (rather than a hand-built world-space frustum)
// because it is guaranteed to match the projection exactly.
// Real textures, lightmaps, entities, sky and water are later milestones.

#include "quakedef.h"
#include <stdio.h>

// Native pipeline (r_native.c) -- no Doom/PS2SDK type clash.
extern void RN_FrameBegin(float camx, float camy, float camz,
                          float pitch, float yaw, float roll);
extern void RN_FrameEnd(void);
extern void RN_SetLight(int l);
extern void RN_SetFovX(float fovx_deg);
extern void RN_TransformToClip(const float *in3, float *out4);
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
#define RGS_WNEAR   0.05f			// near clip: keep clip-space w >= this (>0)

static int          rgs_devtex = -1;
static unsigned int rgs_devpix[RGS_TEXD * RGS_TEXD] __attribute__((aligned(64)));
static int          rgs_visframe = 0;	// our own vis counter (SW path not running)

#ifdef TASTEST
// Per-frame diagnostics, logged to stdio (PCSX2 emulog) periodically.
static int   rgs_dbg_visit, rgs_dbg_clip, rgs_dbg_tris, rgs_dbg_frame;
static int   rgs_dbg_rejnear, rgs_dbg_rejside;
static float rgs_dbg_wmin, rgs_dbg_wmax;
#define DBG(x) x
#else
#define DBG(x)
#endif

// One polygon vertex carried through clipping: homogeneous clip coords (for the
// plane tests), the renderer-world position we actually emit, and texture ST.
typedef struct
{
    float cx, cy, cz, cw;	// clip space (post-MVP, pre-divide)
    float ex, ey, ez;		// renderer-world position (what RN_AddTri wants)
    float s, t;
} rgs_vtx;

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

// Sutherland-Hodgman clip of a convex polygon against one clip-space plane,
// keeping the side where (ax*cx + ay*cy + aw*cw + k) >= 0, interpolating every
// attribute at crossings. out must hold at least n+1 vertices.
static int clip_poly(const rgs_vtx *in, int n,
                     float ax, float ay, float aw, float k, rgs_vtx *out)
{
    int   i, on = 0;
    float da, db, t;

    for (i = 0; i < n; ++i)
    {
        const rgs_vtx *a = &in[i];
        const rgs_vtx *b = &in[(i + 1) % n];

        da = ax*a->cx + ay*a->cy + aw*a->cw + k;
        db = ax*b->cx + ay*b->cy + aw*b->cw + k;

        if (da >= 0.0f)
            out[on++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f))
        {
            t = da / (da - db);
            out[on].cx = a->cx + t*(b->cx - a->cx);
            out[on].cy = a->cy + t*(b->cy - a->cy);
            out[on].cz = a->cz + t*(b->cz - a->cz);
            out[on].cw = a->cw + t*(b->cw - a->cw);
            out[on].ex = a->ex + t*(b->ex - a->ex);
            out[on].ey = a->ey + t*(b->ey - a->ey);
            out[on].ez = a->ez + t*(b->ez - a->ez);
            out[on].s  = a->s  + t*(b->s  - a->s);
            out[on].t  = a->t  + t*(b->t  - a->t);
            on++;
        }
        if (on >= MAXSURFV + 7)
            break;
    }
    return on;
}

// Reconstruct one surface's polygon from its edges, transform to clip space,
// clip against the near + 4 side planes, and emit the survivor as a fan.
// Quake world is (x,y,z) z-up; the renderer wants (x, z, -y).
static void emit_surface(model_t *m, msurface_t *surf)
{
    rgs_vtx	a[MAXSURFV + 8], b[MAXSURFV + 8];
    int		n = surf->numedges, i, cnt;
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
        float		pos[3], clip[4];

        pos[0] =  p[0];		// renderer-world coords (x, z, -y)
        pos[1] =  p[2];
        pos[2] = -p[1];
        RN_TransformToClip(pos, clip);

        a[i].cx = clip[0]; a[i].cy = clip[1]; a[i].cz = clip[2]; a[i].cw = clip[3];
        a[i].ex = pos[0];  a[i].ey = pos[1];  a[i].ez = pos[2];
        DBG(if (clip[3] < rgs_dbg_wmin) rgs_dbg_wmin = clip[3]);
        DBG(if (clip[3] > rgs_dbg_wmax) rgs_dbg_wmax = clip[3]);
        // ST in texels (texinfo maps world->texel); scaled to tile the 64px tex.
        a[i].s = (p[0]*v0[0] + p[1]*v0[1] + p[2]*v0[2] + v0[3]) * (1.0f/64.0f);
        a[i].t = (p[0]*v1[0] + p[1]*v1[1] + p[2]*v1[2] + v1[3]) * (1.0f/64.0f);
    }

    DBG(rgs_dbg_visit++);

    // Clip against near (w>=eps), right (x<=w), left (x>=-w), top (y<=w),
    // bottom (y>=-w). Ping-pong a<->b; the result ends up in b after 5 passes.
    cnt = clip_poly(a, n,    0.0f,  0.0f, 1.0f, -RGS_WNEAR, b); if (cnt < 3) { DBG(rgs_dbg_clip++); DBG(rgs_dbg_rejnear++); return; }
    cnt = clip_poly(b, cnt, -1.0f,  0.0f, 1.0f,  0.0f, a);      if (cnt < 3) { DBG(rgs_dbg_clip++); DBG(rgs_dbg_rejside++); return; }
    cnt = clip_poly(a, cnt,  1.0f,  0.0f, 1.0f,  0.0f, b);      if (cnt < 3) { DBG(rgs_dbg_clip++); DBG(rgs_dbg_rejside++); return; }
    cnt = clip_poly(b, cnt,  0.0f, -1.0f, 1.0f,  0.0f, a);      if (cnt < 3) { DBG(rgs_dbg_clip++); DBG(rgs_dbg_rejside++); return; }
    cnt = clip_poly(a, cnt,  0.0f,  1.0f, 1.0f,  0.0f, b);      if (cnt < 3) { DBG(rgs_dbg_clip++); DBG(rgs_dbg_rejside++); return; }

    DBG(rgs_dbg_tris += cnt - 2);
    for (i = 1; i + 1 < cnt; ++i)
        RN_AddTri(b[0].ex,   b[0].ey,   b[0].ez,   b[0].s,   b[0].t,
                  b[i].ex,   b[i].ey,   b[i].ez,   b[i].s,   b[i].t,
                  b[i+1].ex, b[i+1].ey, b[i+1].ez, b[i+1].s, b[i+1].t);
}

// --- M2c visibility ---------------------------------------------------------
// PVS + backface culling, mirroring WinQuake's R_MarkLeaves / R_RecursiveWorldNode
// (the HW path skips the software R_SetupFrame). Frustum box-culling is dropped:
// clip-space clipping handles the view bounds, and PVS already limits the set.

// Mark every leaf in the view PVS (and the nodes up to the root) visible.
static void rgs_mark_leaves(model_t *m, mleaf_t *viewleaf)
{
    byte	*vis;
    int		i;

    rgs_visframe++;
    vis = Mod_LeafPVS(viewleaf, m);

    for (i = 0; i < m->numleafs; i++)
    {
        if (vis[i >> 3] & (1 << (i & 7)))
        {
            mnode_t *node = (mnode_t *)&m->leafs[i + 1];	// leaf 0 = solid
            do
            {
                if (node->visframe == rgs_visframe)
                    break;
                node->visframe = rgs_visframe;
                node = node->parent;
            } while (node);
        }
    }
}

// Front-to-back BSP walk: mark leaf surfaces, emit visible node surfaces
// (backface-tested), skipping subtrees the PVS pass left unmarked.
static void rgs_walk(model_t *m, mnode_t *node)
{
    mleaf_t		*pleaf;
    msurface_t	*surf;
    double		dot;
    int			c, side;

    if (node->contents == CONTENTS_SOLID)
        return;
    if (node->visframe != rgs_visframe)
        return;

    if (node->contents < 0)		// leaf: mark its surfaces visible
    {
        msurface_t **mark;
        pleaf = (mleaf_t *)node;
        mark = pleaf->firstmarksurface;
        c = pleaf->nummarksurfaces;
        while (c--)
            (*mark++)->visframe = rgs_visframe;
        return;
    }

    // node: which side is the camera on?
    dot  = DotProduct(r_refdef.vieworg, node->plane->normal) - node->plane->dist;
    side = (dot >= 0) ? 0 : 1;

    rgs_walk(m, node->children[side]);		// near side first

    surf = m->surfaces + node->firstsurface;
    for (c = node->numsurfaces; c; c--, surf++)
    {
        if (surf->visframe != rgs_visframe)
            continue;
        if ((dot < 0) ^ ((surf->flags & SURF_PLANEBACK) != 0))
            continue;					// backface
        if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
            continue;					// sky / liquids handled later
        emit_surface(m, surf);
    }

    rgs_walk(m, node->children[side ^ 1]);	// far side
}

// Draw the visible world. Camera comes from r_refdef (set by V_RenderView before
// R_RenderView, so it's valid even though we skip the software R_SetupFrame).
void RGS_RenderWorld(void)
{
    model_t	*m = cl.worldmodel;
    mleaf_t	*viewleaf;
    float	cx, cy, cz, pitch, yaw, roll, hwfov;

    rgs_drew_world = 0;
    if (!m)
        return;

    DBG(rgs_dbg_visit = rgs_dbg_clip = rgs_dbg_tris = 0);
    DBG(rgs_dbg_rejnear = rgs_dbg_rejside = 0);
    DBG(rgs_dbg_wmin = 1e9f); DBG(rgs_dbg_wmax = -1e9f);

    // The native projection's vertical fov reads much narrower than Quake's, so
    // widen it for the HW path (the value the user dialed in).
    hwfov = r_refdef.fov_x * 1.6f;
    if (hwfov > 160.0f) hwfov = 160.0f;

    // Quake view origin/angles -> renderer camera. Angle mapping is a first
    // best-effort (signs/offset likely need a boot iteration or two).
    cx =  r_refdef.vieworg[0];
    cy =  r_refdef.vieworg[2];
    cz = -r_refdef.vieworg[1];
    pitch = -r_refdef.viewangles[0] * DEG2RAD;
    yaw   =  r_refdef.viewangles[1] * DEG2RAD - 1.57079633f;	// flipped: was left-right reversed
    roll  =  r_refdef.viewangles[2] * DEG2RAD;

    RN_SetFovX(hwfov);					// projection (clipping reads it back)
    RN_FrameBegin(cx, cy, cz, pitch, yaw, roll);	// builds the MVP we clip against
    RN_SetLight(128);
    RN_TexBind(rgs_devtex);

    // Visibility: PVS from the view leaf, then a PVS-pruned BSP walk. Clipping
    // (in emit_surface, clip space) handles the view-frustum bounds exactly.
    viewleaf = Mod_PointInLeaf(r_refdef.vieworg, m);
    if (viewleaf)
    {
        rgs_mark_leaves(m, viewleaf);
        rgs_walk(m, m->nodes);
    }
    else
    {
        // No leaf (e.g. outside the map) -> fall back to drawing everything.
        int i;
        for (i = 0; i < m->numsurfaces; ++i)
        {
            msurface_t *surf = &m->surfaces[i];
            if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
                continue;
            emit_surface(m, surf);
        }
    }

    RN_FrameEnd();
    rgs_drew_world = 1;

#ifdef TASTEST
    // Log to stdio (PCSX2 emulog) every ~120 frames: surfaces fed to the clipper,
    // how many got fully clipped away (near vs side), the triangle count, w-range.
    if (++rgs_dbg_frame % 120 == 0)
        printf("GS world: visit=%d rej(near=%d side=%d) tris=%d w[%.1f..%.1f]\n",
               rgs_dbg_visit, rgs_dbg_rejnear, rgs_dbg_rejside, rgs_dbg_tris,
               (double)rgs_dbg_wmin, (double)rgs_dbg_wmax);
#endif
}
