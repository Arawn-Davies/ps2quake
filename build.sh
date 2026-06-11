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

echo ">> running 'make ${*:-all}' in src/ ..."
docker run --rm -v "$ROOT":/work -w /work/src "$IMAGE" make "$@"

if [ "${1:-}" != "clean" ]; then
    echo ">> done: src/bin/quake.elf"
    file "$ROOT/src/bin/quake.elf" 2>/dev/null || true
fi
