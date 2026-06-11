#!/usr/bin/env bash
#
# Build the ps2quake EE executable inside the Dockerized PS2 toolchain.
#
# Builds (or reuses) the image from ./Dockerfile, mounts the repo at /work, and
# runs `make` in src/. Output: src/bin/quake.elf  (a MIPS N32 PS2 EE binary).
#
# Usage:
#   ./build.sh            # build src/bin/quake.elf
#   ./build.sh clean      # remove build artifacts
#   ./build.sh <target>   # run an arbitrary make target (e.g. pack)
#
set -euo pipefail

IMAGE=ps2dock:local
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Build the toolchain image if it's missing (cheap once cached).
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">> building $IMAGE from Dockerfile..."
    docker build -t "$IMAGE" "$ROOT"
fi

# bin/ must exist for the linker output (EE_BIN = bin/quake.elf).
mkdir -p "$ROOT/src/bin"

# Force a clean build for the default target: incremental make over the
# WSL2/Docker bind mount can't be trusted to relink (mtime granularity means
# a fresh .o can look older than the ELF, so the link is silently skipped and
# you ship a stale binary). A full rebuild of this small tree costs seconds.
if [ "$#" -eq 0 ]; then
    echo ">> running 'make clean && make' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" sh -c 'make clean >/dev/null 2>&1; make'
else
    echo ">> running 'make $*' in src/ ..."
    docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" make "$@"
fi

if [ "${1:-}" != "clean" ]; then
    echo ">> done: src/bin/quake.elf"
    file "$ROOT/src/bin/quake.elf" 2>/dev/null || true
fi
