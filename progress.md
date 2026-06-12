# ps2quake — progress

_Last updated: 2026-06-12_

## State

Playable in PCSX2: boots from ISO (`cdfs:`), gsKit video (PSM_T8 + CLUT,
320×224 → 640×448), SFX + IMA-ADPCM music via SPU2/audsrv, DualShock controls.
Builds clean on the modern ps2dev toolchain. CI green.

Today's work: an **EE performance lever** for the software 3D renderer.

## Today: low-res 3D render lever (`r_3dscale`)

Committed in `da3832b` (`src/r_main.c`, `src/r_misc.c`, `src/sys_ps2.c`).

The software rasterizer is the EE bottleneck. Quake couples the 3D render
resolution to the on-screen view size (the rasterizer fills `r_refdef.vrect`
directly in the shared `vid.buffer`), so you can't just shrink the 3D without
shrinking the HUD too — that's the "320-wide HUD floor" that blocked the
earlier internal-resolution attempt.

This lever decouples them:

1. `R_RenderView` (wrapper around `R_RenderView_`) points the rasterizer at a
   **separate low-res buffer** (`vid.buffer`/`vid.rowbytes` swapped) and drives
   `R_ViewChanged` for a `1/N` view rect, so the world is rasterized at a
   fraction of the pixels.
2. `R_ExpandToView()` nearest-expands that low-res image back into `scr_vrect`
   of the full-res `vid.buffer`.
3. The **HUD, console, menus and crosshair are still drawn at full 320 res** on
   top, and the GS still bilinear-upscales the 320-wide frame to 640×448.

Net: only the expensive world fill pays the resolution cut; 2D stays sharp.

### Tuning / fallback

- Cvar **`r_3dscale`** (archived): `1` = off (stock full-res path),
  `2` = half res / quarter the 3D pixels (**default**), `3` = third.
- `scale <= 1` bypasses the whole redirect and calls `R_RenderView_` directly,
  so it's a clean, known-good fallback. Set `r_3dscale 1` at the console to
  compare against (or to disable if anything looks off).

### Measuring

`src/sys_ps2.c` main loop prints `FPS: <n>` to the EE console every 2 s
(below the 60 Hz cap, so it reflects raw render throughput). Use it to A/B
`r_3dscale 1` vs `2` at runtime.

## Gotchas / limitations

- **Water warp is disabled while the low-res pass is active** (`r_misc.c`): the
  warp renders through its own full-width buffer and can't follow the redirect.
  Underwater loses the ripple but stays fast/correct. Re-enable by running at
  `r_3dscale 1`.
- The `r_viewchanged` flag is force-cleared before `R_RenderView_` so
  `R_SetupFrame` doesn't re-run `R_ViewChanged` at full `vid.width` over the
  low-res stride (that would corrupt the buffer). Keep that if you touch the
  wrapper.
- `R_ExpandToView` maps per-axis (index tables), so any view size / odd scale
  stays exact (not assuming a clean 2×).

## Next

1. **Confirm visual + size the prize**: read the `FPS:` log at `r_3dscale 1`
   (baseline) vs `2`/`3`, and eyeball the low-res 3D for artifacts.
2. **Optional quality upgrade — GS composite**: instead of the EE nearest
   expand, upload the low-res 3D as a second GS texture (LINEAR) drawn under the
   2D layer, and alpha-key the 2D layer on Quake's `TRANSPARENT_COLOR` (0xFF,
   which the 2D path already treats as transparent). That gives a smoother
   bilinear 3D upscale and drops the EE expand cost. Plumbing is understood
   (the redirect is the reusable part); the open risk is gsKit TCC/TFX defaults
   for CLUT-alpha keying, which need a runtime check.
3. Deferred: indentation pass over the PS2-port files.

## Build / run

```sh
./build.sh                                   # -> src/bin/quake.elf
./make_iso.sh /mnt/c/Users/azama/Downloads/quake   # -> dist/quake.iso
```

Run is manual (boot `dist/quake.iso` in PCSX2). Assets/builds/logs live in
`C:\Users\azama\Downloads\quake`.
