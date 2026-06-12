#!/usr/bin/env bash
#
# Build a bootable PS2 ISO for ps2quake that PCSX2 (or a real PS2) can run.
#
# The ELF reads its data from the boot disc via the "cdfs:" device, so the
# ISO must contain the game data (id1/). The IOP modules (cdfs, usbd, kbd,
# mouse) are embedded in the ELF by ps2_drivers, so no irx/ is needed on disc.
#
# Layout produced on the disc:
#   /SYSTEM.CNF            -> boots cdrom0:\QUAKE.ELF
#   /QUAKE.ELF            (built by build.sh)
#   /id1/PAK0.PAK [PAK1.PAK]
#
# Usage:
#   ./make_iso.sh <path-to-dir-with-PAK0.PAK[/PAK1.PAK]>
# e.g.
#   ./make_iso.sh /mnt/c/Users/azama/Downloads/quake
#
# Output: dist/quake.iso
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE=ps2dock:local
PAKDIR="${1:-$ROOT/id1}"
STAGE="$ROOT/dist/iso"
OUT="$ROOT/dist/quake.iso"

[ -f "$ROOT/src/bin/quake.elf" ] || { echo "!! src/bin/quake.elf missing -- run ./build.sh first"; exit 1; }

# Locate PAK0.PAK (case-insensitive) in the supplied directory.
pak0="$(find "$PAKDIR" -maxdepth 1 -iname 'pak0.pak' | head -1 || true)"
[ -n "$pak0" ] || { echo "!! pak0.pak not found in $PAKDIR"; exit 1; }
pak1="$(find "$PAKDIR" -maxdepth 1 -iname 'pak1.pak' | head -1 || true)"

echo ">> staging ISO tree in $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/id1"
cp "$ROOT/SYSTEM.CNF"          "$STAGE/SYSTEM.CNF"
cp "$ROOT/src/bin/quake.elf"   "$STAGE/QUAKE.ELF"
cp "$pak0"                     "$STAGE/id1/PAK0.PAK"
[ -n "$pak1" ] && cp "$pak1"   "$STAGE/id1/PAK1.PAK"

# Music tracks for cd_ps2.c (cdfs:/id1/music/trackNN.wav) -- IMA ADPCM WAV,
# decoded on the EE. Look in <pakdir>/id1/music or <pakdir>/music.
musicsrc=""
[ -d "$PAKDIR/id1/music" ] && musicsrc="$PAKDIR/id1/music"
[ -d "$PAKDIR/music" ]     && musicsrc="$PAKDIR/music"
if [ -n "$musicsrc" ] && ls "$musicsrc"/*.wav >/dev/null 2>&1; then
    mkdir -p "$STAGE/id1/music"
    cp "$musicsrc"/*.wav "$STAGE/id1/music/"
    echo ">> staged $(ls "$STAGE/id1/music"/*.wav | wc -l) WAV music track(s)"
fi

echo ">> building $OUT"
mkdir -p "$ROOT/dist"
# -as mkisofs: plain ISO9660 (level: allow our 8.3 names). PS2 cdvd/cdfs read
# standard ISO9660; cdfs is case-insensitive so lowercase cdfs:/ paths match.
docker run --rm -v "$ROOT":/work -w /work "$IMAGE" \
    xorriso -as mkisofs -iso-level 2 -l -o dist/quake.iso dist/iso

echo ">> done: dist/quake.iso"
ls -la "$OUT"
