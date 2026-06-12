# ps2quake

A PlayStation 2 port of **Quake**, originally written by Nicolas Plourde
(~2004), revived to build and run on the **modern [ps2dev](https://github.com/ps2dev)
toolchain** and boot from a CD/DVD image (so it runs in **PCSX2** and on real
hardware). The original port rendered video only — no sound, no controller,
loaded from `host:` over a link cable. This fork is now a complete, playable
build.

> The plain-text `README` in this repo is Nicolas's original 2004 release note,
> kept for history.

## What works

- **Boots from an ISO** (data on the disc, read via `cdfs:`) — no link cable,
  no USB stick required.
- **Hardware-assisted video**: the software renderer draws at 320×224 and the
  **GS** upscales it to 640×448 (PSM_T8 texture + CLUT, bilinear).
- **Sound effects**: Quake's software mixer streamed to the **SPU2** via
  `audsrv`.
- **Music**: the CD soundtrack as **IMA-ADPCM WAV**, streamed off the disc and
  decoded on the EE (cheap), mixed with the SFX.
- **DualShock controller** (dual-stick), plus USB keyboard/mouse if present.

Tested primarily in PCSX2.

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

A USB keyboard and mouse also work if connected.

## Building

You need **Docker** — the build runs inside the official ps2dev image (the
`Dockerfile` adds `make`/`bash`/`xorriso`). No local toolchain required.

```sh
./build.sh            # -> src/bin/quake.elf  (MIPS N32 PS2 EE binary)
./build.sh clean
```

## Making a bootable disc

You supply the **game data** (this repo ships none):

- `PAK0.PAK` — Quake **shareware** (free, redistributable) or **retail**
  `PAK0.PAK` (+ `PAK1.PAK`).
- *(optional)* **music** — `track02.wav` … `track11.wav` in an `id1/music/`
  folder, each **22050 Hz stereo IMA-ADPCM WAV**:

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

- **PCSX2**: boot `dist/quake.iso` (a PAL-region BIOS works; enable Fast Boot
  if it drops to the browser).
- **Real PS2**: burn the ISO or load it with your launcher of choice. cdfs,
  SPU2 audio and libpad all target real hardware too, though it hasn't been
  extensively tested there yet.

## Credits

- **Nicolas Plourde** — original 2004 PS2 Quake port (`sys_ps2`, `vid_ps2`,
  `in_ps2`, `ps2_gs`, `pad`). PS2 code based on the Dreamtime tutorial.
- **id Software** — *Quake* (GPLv2). Stock engine + sound mixer from the
  id-Software/Quake GPL release.
- Modern toolchain glue uses **[ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk)**,
  **gsKit**, **audsrv**, and **[fjtrujy/ps2_drivers](https://github.com/fjtrujy/ps2_drivers)**.

Licensed under the **GNU GPL v2** (see `gnu.txt`).
