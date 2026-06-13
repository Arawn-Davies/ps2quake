#!/usr/bin/env bash
#
# Build the ps2quake EE executable inside the Dockerized PS2 toolchain.
#
# Builds (or reuses) the image from ./Dockerfile, mounts the repo at /work, and
# runs `make` in src/. Output: src/bin/quake.elf  (a MIPS N32 PS2 EE binary).
#
# Usage:
#   ./build.sh            # software renderer -> src/bin/quake.elf
#   ./build.sh hw         # GS hardware renderer -> src/bin/quake-hw.elf
#   ./build.sh launcher   # boot picker -> src/bin/launcher.elf
#   ./build.sh clean      # remove build artifacts
#   ./build.sh <target>   # run an arbitrary make target (e.g. pack)
#
# Software and hardware are separate ELFs (separate renderers, so each binary
# only carries its own buffers). 'make_iso.sh <pak> combo' bundles both plus the
# launcher onto one selectable disc -- run ./build.sh, ./build.sh hw and
# ./build.sh launcher first.
set -euo pipefail

IMAGE=ps2dock:local
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Build the toolchain image if it's missing (cheap once cached).
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">> building $IMAGE from Dockerfile..."
    docker build -t "$IMAGE" "$ROOT"
fi

# bin/ must exist for the linker output.
mkdir -p "$ROOT/src/bin"

# A clean build is forced whenever the renderer (compile-time gated) is built,
# because incremental make over the WSL2/Docker bind mount can't be trusted to
# relink, and toggling -DR_HARDWARE must recompile the gated files. A full
# rebuild of this small tree costs seconds.
OUT="src/bin/quake.elf"
if [ "${1:-}" = "hw" ]; then
    echo ">> running 'make clean && make R_HARDWARE=1' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" sh -c 'make R_HARDWARE=1 clean >/dev/null 2>&1; make R_HARDWARE=1'
    OUT="src/bin/quake-hw.elf"
elif [ "${1:-}" = "launcher" ]; then
    echo ">> running 'make -f Makefile.launcher' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" sh -c 'make -f Makefile.launcher clean >/dev/null 2>&1; make -f Makefile.launcher'
    OUT="src/bin/launcher.elf"
elif [ "$#" -eq 0 ]; then
    echo ">> running 'make clean && make' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" sh -c 'make clean >/dev/null 2>&1; make'
else
    echo ">> running 'make $*' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" make "$@"
fi

if [ "${1:-}" != "clean" ]; then
    echo ">> done: $ROOT/$OUT"
    file "$ROOT/$OUT" 2>/dev/null || true
fi
