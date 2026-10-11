#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The dub preamp emulator for the browser: tools/web_emu/emu.c (SLOOP + the preamp, the host build of the tests)
# compiled to WebAssembly with Emscripten in Docker (nothing installed on the Mac): tools/web_emu/preamp_emu.js and
# preamp_emu.wasm, loaded by tools/web_emu/index.html.
#   tools/web_emu/build_emu.sh
# Needs Docker, and build/gen (the generated headers: ./build.sh makes them, or tools/build.py's generate step).
#   EMSDK_IMAGE  the Emscripten image (default emscripten/emsdk:3.1.74; linux/amd64, Rosetta on Apple silicon)
set -e
cd "$(dirname "$0")/../.."
IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:3.1.74}"
[ -f build/gen/felucca_tables.h ] || { echo "build_emu.sh: no build/gen (run ./build.sh first)"; exit 1; }
command -v docker >/dev/null 2>&1 || PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
docker info >/dev/null 2>&1 || { echo "build_emu.sh: Docker is not running"; exit 1; }
EXPORTS='["_emu_init","_emu_render","_emu_frame","_emu_pad","_emu_knob","_emu_note","_emu_play","_emu_cc","_emu_pads_lit","_emu_playing","_emu_band"]'
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" \
    emcc -O2 -w -DENABLE_DUB_PREAMP -Ibuild/gen -Ifirmware/src -Ifirmware/hal \
    tools/web_emu/emu.c -o tools/web_emu/preamp_emu.js \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=PreampEmu -sENVIRONMENT=web,node \
    -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS="$EXPORTS" \
    -sEXPORTED_RUNTIME_METHODS='["HEAPU16","HEAPF32"]'
ls -l tools/web_emu/preamp_emu.js tools/web_emu/preamp_emu.wasm
