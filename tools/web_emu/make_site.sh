#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The public web console as a static site (GitHub Pages: .github/workflows/deploy.yml; or any static host):
# tools/web_emu/ and what it loads, laid out as in the repository so every relative path the page uses
# (../../web/fm1ota.js, ../../build/felucca.fwsc, ./pkg.json, preamp_emu.wasm) resolves unchanged, plus an index
# at the root that opens the console. Then site_check.mjs walks the page's references: a missing file fails here,
# not as a 404 on the site.
#   tools/web_emu/make_site.sh [OUT]          (default build/site; after build_emu.sh)
# Without build/felucca.fwsc the site ships no package and pkg.json says so (the installer stays off).
set -e
cd "$(dirname "$0")/../.."
OUT="${1:-build/site}"
E=tools/web_emu
for f in "$E/preamp_emu.js" "$E/preamp_emu.wasm"; do
    [ -f "$f" ] || { echo "make_site.sh: no $f (run tools/web_emu/build_emu.sh first)"; exit 1; }
done
node "$E/pkg_manifest.mjs" >/dev/null                   # the pin of what ships (or of no package)
rm -rf "$OUT"
mkdir -p "$OUT/$E" "$OUT/web"
cp "$E/index.html" "$E/app.js" "$E/skin.js" "$E/audio_shield.js" "$E/installer.js" \
   "$E/preamp_emu.js" "$E/preamp_emu.wasm" "$E/pkg.json" "$OUT/$E/"
cp web/fm1ota.js web/fm1pkg.js "$OUT/web/"             # SLOOP's WebMIDI installer scripts, unchanged
if [ -f build/felucca.fwsc ]; then
    mkdir -p "$OUT/build"
    cp build/felucca.fwsc "$OUT/build/"
fi
cat > "$OUT/index.html" <<'HTML'
<!doctype html>
<!-- SPDX-License-Identifier: GPL-3.0-only -->
<html lang="en"><head><meta charset="utf-8"><title>Dub Preamp Sound System</title>
<link rel="icon" href="data:,">
<meta http-equiv="refresh" content="0; url=tools/web_emu/">
</head><body style="background:#121212;color:#fabc00;font:14px ui-monospace,monospace">
<p><a href="tools/web_emu/" style="color:#fabc00">Dub Preamp Sound System: the FM-1 console</a></p></body></html>
HTML
touch "$OUT/.nojekyll"                                  # (served as is: no Jekyll pass)
node "$E/site_check.mjs" "$OUT"
