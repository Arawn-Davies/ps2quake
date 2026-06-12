# ps2quake

A native **PlayStation 2** port of id Software's **Quake**. Boots from a CD/DVD
image, runs in **PCSX2** and on real hardware — video, sound, music, and
controller all working.

The 3D world is software-rendered today; a **GS hardware renderer** (VU1 + DMA +
the Graphics Synthesizer) is in progress — see [`docs/gs-renderer.md`](docs/gs-renderer.md).

## Features

- **Boots from an ISO** — game data on the disc, read via `cdfs:`. No link
  cable, no USB stick.
- **Video** — software renderer at 384×268, GS-upscaled to 640×448 (PSM_T8
  texture + CLUT, bilinear). `r_3dscale` trades sharpness for speed.
- **Sound** — Quake's software mixer streamed to the **SPU2** via `audsrv`.
- **Music** — the CD soundtrack as IMA-ADPCM WAV, streamed off the disc and
  decoded on the EE, mixed with the SFX.
- **Controller** — DualShock dual-stick, plus USB keyboard/mouse if connected.

## Controls

| Input | Action |
|-------|--------|
| Left stick | Move / strafe |
| Right stick | Look |
| R1 / R2 | Fire |
| Square | Jump |
| Circle | Run |
| Cross (✕) | Menu confirm |
| Triangle / Start | Menu / back |
| Select | Scoreboard |
| D-pad | Menu navigation / move |

## Building

Requires **Docker** — the build runs inside the official ps2dev image (the
`Dockerfile` adds `make`/`bash`/`xorriso`). No local toolchain needed.

```sh
./build.sh            # -> src/bin/quake.elf  (MIPS N32 PS2 EE binary)
./build.sh clean
```

## Making a bootable disc

You supply the **game data** (this repo ships none):

- `PAK0.PAK` — Quake **shareware** (free, redistributable) or **retail**
  `PAK0.PAK` (+ `PAK1.PAK`).
- *(optional)* **music** — `track02.wav` … `track11.wav` in an `id1/music/`
  folder, each 22050 Hz stereo IMA-ADPCM WAV:

  ```sh
  ffmpeg -i track02.ogg -ar 22050 -ac 2 -c:a adpcm_ima_wav track02.wav
  ```

Point `make_iso.sh` at the directory holding the pak(s) (and `id1/music/`):

```sh
./make_iso.sh /path/to/quake     # -> dist/quake.iso
```

The disc lays out `SYSTEM.CNF` (boots `cdrom0:\QUAKE.ELF`), `QUAKE.ELF`,
`id1/PAK0.PAK` (+`PAK1.PAK`), and `id1/music/*.wav`.

## Running

- **PCSX2** — boot `dist/quake.iso`. A PAL-region BIOS works; enable Fast Boot
  if it drops to the system browser.
- **Real PS2** — burn the ISO or load it with your launcher of choice. `cdfs`,
  SPU2 audio, and `libpad` all target real hardware; not yet extensively tested
  there.

## Roadmap

The current bottleneck is EE software rasterization. The next major effort is a
GS hardware geometry renderer that moves world rasterization onto the GPU
(native 640×512, hardware Z-buffer). Full design, pipeline diagrams, and the
milestone plan: [`docs/gs-renderer.md`](docs/gs-renderer.md).

## Credits

- **id Software** — *Quake* and the GPL engine release (GPLv2).
- Built on **[ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk)**, **gsKit**,
  **audsrv**, and **[fjtrujy/ps2_drivers](https://github.com/fjtrujy/ps2_drivers)**.

Licensed under the **GNU GPL v2** (see `gnu.txt`).
