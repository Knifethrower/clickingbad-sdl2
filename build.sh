#!/bin/sh
# Build Clicking Bad: ./build.sh  ->  clickingbad.<arch>
# Needs SDL2 (libsdl2-dev), a C compiler and Node.js (data.h is generated from the original game.js).
# Cross build against an arm64 sysroot (e.g. Debian 11 for PortMaster): SYSROOT=/path/to/sysroot ./build.sh
set -e
cd "$(dirname "$0")"
[ -f upstream/templates/game.js ] || git submodule update --init
node gen_data.js upstream/templates/game.js > data.h
CFLAGS="-O2 -Wall -Wextra -Wno-unused-function -std=gnu99"
if [ -n "$SYSROOT" ]; then
    clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld $CFLAGS -I"$SYSROOT/usr/include/SDL2" -D_REENTRANT \
        -o clickingbad.aarch64 main.c -L"$SYSROOT/usr/lib/aarch64-linux-gnu" -lSDL2 -lm
else
    ${CC:-cc} $CFLAGS -o clickingbad.$(uname -m) main.c $(sdl2-config --cflags --libs) -lm
fi
