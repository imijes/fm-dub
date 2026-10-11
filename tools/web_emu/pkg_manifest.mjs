// SPDX-License-Identifier: GPL-3.0-only
// Pins the build for the web console's installer (installer.js loadPackage): tools/web_emu/pkg.json with the
// package's identity, size and SHA-256, as Hortator's make_site.py pins meta.sha256. The page writes only that exact
// file: run this after every ./build.sh (build_emu.sh runs it too). Without a package it pins none
// ({ available: false }): the installer then says so, and never asks for the file.
//   node tools/web_emu/pkg_manifest.mjs [build/felucca.fwsc]
import { readFileSync, writeFileSync, statSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";
import { productOf } from "../../web/fm1pkg.js";

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, "..", "..");
const pkgPath = process.argv[2] || join(root, "build", "felucca.fwsc");
const out = join(here, "pkg.json");
if (!existsSync(pkgPath)) {                            // a site without a package (tools/web_emu/make_site.sh): said so
  const pin = { available: false, reason: "no firmware package in this deployment: build it (ENABLE_DUB_PREAMP=1 ./build.sh) to install from here" };
  writeFileSync(out, JSON.stringify(pin, null, 2) + "\n");
  console.log(`pkg.json: no ${relative(root, pkgPath)}: the installer says there is no package`);
  process.exit(0);
}
const pkg = readFileSync(pkgPath);
const product = productOf(pkg);
if (!/^FM-1_9\d\d$/.test(product)) {
  console.error(`${pkgPath}: not a SLOOP package (${product || "no identity"})`);
  process.exit(1);
}
const pin = {
  available: true,
  product,
  size: pkg.length,
  sha256: createHash("sha256").update(pkg).digest("hex"),
  built: statSync(pkgPath).mtime.toISOString(),
};
writeFileSync(out, JSON.stringify(pin, null, 2) + "\n");
console.log(`pkg.json: ${relative(root, pkgPath)} ${pin.product}, ${pin.size} B, sha256 ${pin.sha256}`);
