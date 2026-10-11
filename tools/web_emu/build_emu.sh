#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The dub preamp FM-1 for the browser: tools/web_emu/emu.c (the whole firmware: SLOOP, its UI, the preamp; the host build of the tests)
# compiled to WebAssembly with Emscripten in Docker (nothing installed on the Mac): tools/web_emu/preamp_emu.js and
# preamp_emu.wasm, loaded by tools/web_emu/index.html; then pins build/felucca.fwsc for its installer (pkg.json).
#   tools/web_emu/build_emu.sh
# Needs Docker, and build/gen (the generated headers: ./build.sh makes them, or tools/build.py's generate step).
#   EMSDK_IMAGE  the Emscripten image (default emscripten/emsdk:3.1.74; linux/amd64, Rosetta on Apple silicon)
set -e
cd "$(dirname "$0")/../.."
IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:3.1.74}"
[ -f build/gen/felucca_tables.h ] || { echo "build_emu.sh: no build/gen (run ./build.sh first)"; exit 1; }
command -v docker >/dev/null 2>&1 || PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
docker info >/dev/null 2>&1 || { echo "build_emu.sh: Docker is not running"; exit 1; }
EXPORTS='["_emu_init","_emu_render","_emu_ui_frames","_emu_btn","_emu_key","_emu_enc","_emu_master","_emu_screen","_emu_leds","_emu_key_leds","_emu_playing","_emu_band","_emu_keymap","_emu_state"]'
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" \
    emcc -O2 -w -DENABLE_DUB_PREAMP -Ibuild/gen -Ifirmware/src -Ifirmware/hal \
    tools/web_emu/emu.c -o tools/web_emu/preamp_emu.js \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=PreampEmu -sENVIRONMENT=web,node \
    -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS="$EXPORTS" \
    -sEXPORTED_RUNTIME_METHODS='["HEAP8","HEAPU8","HEAPU16","HEAPF32"]'
ls -l tools/web_emu/preamp_emu.js tools/web_emu/preamp_emu.wasm
if [ -f build/felucca.fwsc ] && command -v node >/dev/null 2>&1; then
    node tools/web_emu/pkg_manifest.mjs                 # the installer's pin of the build (installer.js)
fi
