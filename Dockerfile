# Dockerized PlayStation 2 build environment for the ps2quake port.
#
# The upstream ps2dev image ships the full PS2 cross-toolchain
# (mips64r5900el-ps2-elf-gcc, ps2sdk, gsKit, audsrv) and all the env vars
# (PS2DEV, PS2SDK, GSKIT, PATH) preconfigured -- but it's a minimal Alpine
# image without `make`. We add just enough to drive the toolchain.
#
# Build the port with ./build.sh (see that script / README), which mounts the
# repo and runs `make` in src/.
FROM ghcr.io/ps2dev/ps2dev:latest

RUN apk add --no-cache make bash

# The Makefile lives in src/; the repo is mounted at /work by build.sh.
WORKDIR /work/src
