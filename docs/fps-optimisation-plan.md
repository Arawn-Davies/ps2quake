# ps2quake — FPS optimisation plan

*Status: planning note (no code changed). Companion to [`gs-renderer.md`](gs-renderer.md).*

## TL;DR

- The **present path is already optimal** — the 8-bit software frame is handed to
  the GS as **PSMT8 + CT32 CLUT** and the GS does the palette-expand **and** the
  upscale to 640×448 in hardware (`vid_ps2.c` lines 23–28). The EE never expands
  pixels. This is the same trick that makes ps2oom's gsKit renderer full-speed,
  so it can't be the source of more wins here — it's already done.
- The bottleneck is the **EE software rasteriser** (`d_*.c`). It is
  **memory-latency-bound**, not compute-bound: `R_EdgeDrawing` (spans + surface
  cache) runs ~25 ms (light) / 42–53 ms (heavy) per frame, thrashing the EE's
  8 KB data cache on the 64 KB colormap + texture/lightmap gathers
  (`docs/gs-renderer.md` §1). Default render res is 384×268 at `-O2`.
- **The PS2 can absolutely run Quake at 60 fps** — Quake III Revolution,
  Half-Life PS2, TimeSplitters 1/2/FP (60 fps), Black, Killzone, Red Faction all
  prove it. **None of them rasterise 3D on the EE core.** They all use
  **VU1 (transform/clip/light) → GS (rasterise/texture/Z)**. The EE is a feeder.
- **We already have most of that pipeline**: `r_native.c` + `draw_3D.vsm` (VU1) +
  `r_gs.c`. The **geometry feed works** — the BSP world is walked and fed to
  VU1 → GS, with the GS hardware Z-buffer doing occlusion (M2b). Two missing-
  geometry sources, though: **ps2gl** drops geometry by design (silent overflow,
  not used here), **and** a clip-space clipper (M2c/M2d: PVS + near/side-plane
  clip) that was tried **but has since been reverted** because the clip planes
  over-rejected surfaces — i.e. that clipper was itself a *cause* of missing
  walls. So **correct clipping is still an open problem**, not a solved one.
- So there are two tracks. **Track A** squeezes the software renderer (no
  rewrite) and gets it *playable*. **Track B** finishes the native GS+VU renderer
  (textures + lightmaps) and gets it *fast* — and it's ~60–70 % built already.

---

## 1. Diagnosis

### What's already solved
`vid_ps2.c` reuses the ps2oom gsKit approach exactly: `vid.buffer` (8-bit
indexed) → GS **PSM_T8** texture + **CT32** CLUT → GS does palette expansion +
bilinear scale to the 640×448 display. The EE does **zero** per-pixel expansion.
There is nothing left to win in the *present* path.

### Why it's still slow
Quake's software rasteriser is far heavier than Doom's:

- perspective-correct texture mapping (per-span divides),
- per-surface **lightmaps** + **surface caching** (build a lit texture, then map it),
- a **z-buffer**,
- alias-model affine triangles (`d_polyse.c`).

The port's own profiling (`docs/gs-renderer.md` §1):

| metric | light scene | heavy dip |
|---|---|---|
| world raster (`V_RenderView`) | ~31 ms | ~55–80 ms |
| └ `R_EdgeDrawing` (spans + surface cache) | ~25 ms | 42–53 ms |
| GS present (upload + blit) | ~10 ms | ~10 ms |

Root cause, quoted from the doc: *"memory-latency-bound — the 64 KB colormap +
texture/lightmap gathers thrash the EE's 8 KB data cache, which assembly cannot
fix."*

### The reframe that matters
The earlier "ceiling" applies to **one approach only**: id's Pentium-era software
span loop running on a MIPS core that is the wrong tool for it. It says nothing
about the PS2's headroom. The PS2's rendering muscle is **VU1 + GS**, not the
R5900 core:

- **VU1** — standalone SIMD vector unit, ~3.2 GFLOPS, 16 KB micro-mem + 16 KB
  data RAM. Does transform, clipping, lighting, perspective divide; emits GS
  primitives. (VU0 assists.)
- **GS** — 2.4 Gpixel/s, 16 pixel pipelines, HW texture mapping + bilinear + Z +
  alpha + fog, fed at ~48 GB/s. **Only real constraint: 4 MB embedded VRAM**
  (textures must be cached/paged).

Every fast PS2 FPS uses: **EE walks the world & builds DMA chains → VU1
transforms/clips/lights → GIF → GS rasterises textured, Z-buffered triangles.**
That is GLQuake's model with VU1 in the place of a fixed-function GPU.

---

## 2. Track A — accelerate the software renderer (no new renderer)

Gets it *playable*. Ranked by return-on-effort.

### A1. Default `r_3dscale = 2`, expose a Quality/Speed slider — **do this first**
The true transferable ps2oom lesson: *render small, let the GS upscale.*
`r_3dscale` (`r_main.c`) already renders the **3D world** into a low-res buffer
(192×134 at scale 2 — a **4× pixel-fill reduction**) and GS bilinear-upscales it
(`tex3d`, LINEAR) to the view rect, while the **2D/HUD stays sharp** at 384×268.
The span-fill cost (the part that scales with pixels) drops hard.

- Make scale 2 the default; expose **1 = sharp/slow, 2 = soft/fast, 3 = fastest**
  in `ps2_settings.c`.
- Near-zero risk, already implemented. Likely **1.5–2.5×** in light scenes.

### A2. Offload the cheap-to-move passes to the GS
Partial hardware, **reusing the existing native triangle core** — not a rewrite:

- **Sky** (`d_sky.c`) — software sky warp is expensive; draw it as a GS
  textured/scrolled quad.
- **Particles** (`d_part.c`) — plotted pixel-by-pixel on the EE today; emit them
  as **GS point-sprites**.
- **Alias models** (`d_polyse.c`, weapon + monsters) — feed *just* alias models
  through the existing `r_native.c` / `draw_3D.vsm` VU1 triangle path (geometry +
  GS Z-buffer already work) while the world stays software. Small, convex,
  near-camera meshes also sidestep the unsolved world-clipping issue.

Removes the most expensive software passes without touching the BSP/world raster.

### A3. Attack the latency bottleneck with the EE scratchpad (SPR)
The PS2-specific fix for the exact problem the doc names. The EE has **16 KB of
fast on-chip Scratchpad RAM**. Stage the *current* surface's lit-cache block (and
/or lightmap + texture) into SPR via DMA, and run `D_DrawSpans` / `D_CacheSurface`
reading from SPR instead of cache-missing main RAM. **Double-buffer**: DMA the
*next* surface's data into SPR while rasterising the current one, hiding the
latency the doc blames for 42–53 ms. Highest-effort Track-A item, but it targets
the root cause; even partial staging of the hot gathers should help.

### A4. Cheap build/loop wins
- Bump hot files (`d_scan.c`, `d_edge.c`, `d_polyse.c`, `d_sky.c`, `d_surf.c`) to
  **`-O3`** (ps2oom does this for `r_draw/r_plane/r_things`).
- Add explicit **`pref` prefetch** of texture/surface rows ahead of the span loop.
- **Enlarge the surface cache** (32 MB RAM available; more cache = fewer re-lights).
- **Frame-pace** to a cap so cycles aren't wasted.

Modest individually, stackable, low-risk.

### Track-A honest ceiling
A1 + A2 can plausibly lift heavy scenes from ~12–18 fps into the playable 25–40
range and light scenes near 60 — but software Quake on the EE will never be
GLQuake-fast. The memory subsystem is the wall. **For real speed, see Track B.**

---

## 3. Track B — finish the native GS+VU renderer (the road to 60 fps)

This is **not** a new renderer and **not** ps2gl. It's finishing the one that
already exists.

### What's already working (M2b)
`r_native.c` + `draw_3D.vsm` (VU1) + `r_gs.c` is the VU1→GS pipeline, with the
**geometry feed proven**:

- VU1 transform + perspective divide; full **BSP→GS geometry feed** (every world
  surface emitted as triangles).
- **GS hardware Z-buffer** doing occlusion.
- In-progress now (uncommitted WIP): **double-buffering + a GS `FINISH`/vsync
  flip** in `RN_FrameEnd` to kill the "flicker while turning", and a `TASTEST`
  harness (scripted camera + boot-to-`e1m1`) to A/B the software vs hardware
  renderers on identical views.

### What's NOT solved yet — clipping
A clip-space clipper (M2c PVS cull + M2d Sutherland–Hodgman near/side-plane
clipping in `clip_poly`) was implemented **and then reverted back to M2b**,
because the clip planes/MVP over-rejected surfaces — that over-rejection is a
likely *cause* of the "missing walls/floors/ceilings", not a fix for it. So
**correct world clipping is an open problem.** (Note: ps2gl is a *separate*
missing-geometry trap — it silently drops geometry on packet-buffer overflow;
we abandoned it on ps2oom and the hand-rolled core avoids that, but it doesn't
make ps2quake's clipping correct on its own.)

Right now the renderer leans on the **GS Z-buffer + trivial VU reject** with no
geometric clipping, which works for many views but can break at the near plane /
guard band. Getting clipping right (or proving the GS guard band + a correct
near-plane-only clip is enough) is a prerequisite for a robust hardware renderer.

### What's left = M3+
It currently draws a **dev checkerboard** on every surface. The remaining work:

0. **Robust clipping** (see above) — the open geometry correctness item.
1. **Textures: PSMT8 + CLUT in VRAM + an LRU texture cache.** ← centrepiece.
   Quake's textures are **8-bit palettised**, so store them on the GS as
   **PSMT8 + CLUT** — the same trick as the framebuffer — for **¼ the VRAM** of
   RGBA (a 256×256 wall: 256 KB → 64 KB). That makes the working set fit the 4 MB
   budget; an **LRU texture cache** (upload the visible set per frame via DMA)
   closes the rest. This dissolves the VRAM blocker.
2. **Lightmaps** — packed into atlas textures (GLQuake-style), applied as a
   second GS pass (multiply/modulate blend) or combined. Lightmaps are tiny
   (~16×16 luxels/surface).
3. **Alias models** through the existing VU1 triangle path (weapon + monsters).
4. **Sky** (GS quad) and **water** (GS warp/scroll).
5. **HUD / console overlay** on top (M7).

The software accelerations from Track A become a **fallback "compatibility"
renderer** for cases the hardware path doesn't cover yet — not the destination.

---

## 4. Recommended sequence

1. **`r_3dscale = 2` default + Quality/Speed slider** — minutes of work, biggest
   immediate win, pure GS-upscale. **Measure.**
2. **`-O3` hot files + surface-cache enlarge + frame cap** — quick, low-risk.
   **Measure.**
3. **GS-offload sky → particles → alias models** through the native core —
   removes the worst software passes, no world rewrite.
4. If still short of 60 fps (it will be, in heavy scenes): **commit to Track B —
   the native GS renderer.** First land the in-progress double-buffer/vsync fix,
   then **solve clipping** (M2d redo or a proven near-plane-only + guard-band
   approach — use the `TASTEST` A/B harness to verify against the software view),
   then **M3** (textures PSMT8+CLUT + texture cache, then lightmaps). This is the
   only path to GLQuake-class FPS; the geometry feed + GS Z-buffer spine is built,
   clipping correctness is the gating problem.

---

## 5. References

- **GLQuake source** — the exact lightmap/texture model to mirror.
- **QuakeSpasm / vkQuake** — clean, modern lightmap-atlas + texture handling.
- **PS2SDK `draw` / `packet2` + VU examples** — the HW triangle + DMA path.
- **GoldSrc / Half-Life** — it *is* the Quake engine; the PS2 HL port is proof
  the design works, not a new design.
- **[`gs-renderer.md`](gs-renderer.md)** — the native renderer's milestone roadmap
  (M1–M8), profiling, VRAM budget, and the RN_* / RGS_* API split.

## 6. Bottom line

The present path is already GS-accelerated (the ps2oom win, already here). The
software rasteriser is the wall, and it's memory-bound, so squeezing it (Track A)
buys *playable*, not *fast*. Real speed lives where every shipped PS2 FPS put it:
**VU1 + GS** — which this port already has the spine of: a working geometry feed
+ GS Z-buffer (M2b). The gating problem is **correct clipping** (the M2d clipper
was reverted for over-rejecting surfaces), after which **textures + lightmaps**
(M3) get you to GLQuake class. ps2gl is not the path (it drops geometry); the
hand-rolled core is — it just needs clipping finished and texturing added.
