// SPDX-License-Identifier: GPL-3.0-only
// The public site's reference check (make_site.sh runs it, the deploy workflow too): from the root index, every
// page, script, module import (static and dynamic), the Emscripten loader's .wasm, and the runtime fetches app.js
// makes (its PKG_URL / PIN_URL constants) must resolve to a file in the site; the pin must match the package that
// ships (or say there is none). Prints what ships and its sizes; with GITHUB_STEP_SUMMARY set, writes them there.
//   node tools/web_emu/site_check.mjs build/site
import { readFileSync, existsSync, statSync, readdirSync, appendFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { gzipSync } from "node:zlib";
import { join, dirname, normalize, relative, posix } from "node:path";

const site = process.argv[2] || "build/site";
const errors = [], seen = new Set();
const isLocal = (u) => !/^(?:[a-z]+:|\/\/|#)/i.test(u);
const resolve = (from, u) => normalize(join(dirname(from), u.split(/[?#]/)[0]));
function need(from, u, why) {
  if (!isLocal(u)) return null;
  let f = resolve(from, u);
  if (u.endsWith("/")) f = join(f, "index.html");
  const rel = relative(site, f);
  if (rel.startsWith("..")) { errors.push(`${relative(site, from)}: ${u} (${why}) points outside the site`); return null; }
  if (!existsSync(f) || statSync(f).isDirectory()) { errors.push(`${relative(site, from)}: ${u} (${why}) -> ${rel}: missing`); return null; }
  return f;
}
function walk(file) {
  if (!file || seen.has(file)) return;
  seen.add(file);
  const text = readFileSync(file, "utf8");
  if (file.endsWith(".html")) {
    for (const m of text.matchAll(/<(?:script|img|link|a)\b[^>]*?\b(?:src|href)="([^"]+)"/g)) walk(need(file, m[1], "html"));
    for (const m of text.matchAll(/http-equiv="refresh" content="\d+;\s*url=([^"]+)"/g)) walk(need(file, m[1], "refresh"));
  } else if (file.endsWith(".js") || file.endsWith(".mjs")) {
    // (a bare specifier, "module": a Node built-in of the Emscripten loader's node branch, not a file of the site)
    const rel = (u) => /^\.{0,2}\//.test(u);
    for (const m of text.matchAll(/\bimport\s+(?:[^"';]*?\bfrom\s+)?["']([^"']+)["']/g)) if (rel(m[1])) walk(need(file, m[1], "import"));
    for (const m of text.matchAll(/\bimport\(\s*["']([^"']+)["']\s*\)/g)) if (rel(m[1])) walk(need(file, m[1], "import()"));
    for (const m of text.matchAll(/["']([\w.-]+\.wasm)["']/g)) need(file, m[1], "wasm");
    for (const m of text.matchAll(/\b(PKG_URL|PIN_URL)\s*=\s*["']([^"']+)["']/g)) {
      if (m[1] === "PKG_URL") pkgRef = { from: file, url: m[2] };
      else need(file, m[2], "fetch");
    }
  }
}
let pkgRef = null;
walk(need(join(site, "x"), "index.html", "root"));

/* the pin and the package */
const pinFile = join(site, "tools/web_emu/pkg.json");
let pin = null, pkgLine = "no firmware package (the installer says so)";
if (existsSync(pinFile)) {
  pin = JSON.parse(readFileSync(pinFile, "utf8"));
  if (pin.available !== false) {
    const f = pkgRef && need(pkgRef.from, pkgRef.url, "fetch: the package");
    if (f) {
      const b = readFileSync(f), sha = createHash("sha256").update(b).digest("hex");
      if (sha !== pin.sha256 || b.length !== pin.size) errors.push(`pkg.json does not pin ${relative(site, f)} (sha256 ${sha.slice(0, 12)})`);
      pkgLine = `${pin.product}, ${b.length} B, sha256 ${sha}`;
    }
  } else if (pkgRef && existsSync(resolve(pkgRef.from, pkgRef.url))) {
    errors.push("pkg.json says no package, but one ships");
  }
} else errors.push("tools/web_emu/pkg.json: missing (the installer would 404)");

/* what ships */
const files = [];
(function list(d) { for (const e of readdirSync(d, { withFileTypes: true })) { const p = join(d, e.name); e.isDirectory() ? list(p) : files.push(p); } })(site);
const rows = files.map((f) => {
  const b = readFileSync(f);
  return { path: relative(site, f).split("\\").join(posix.sep), size: b.length, gz: gzipSync(b, { level: 9 }).length, used: seen.has(f) };
}).sort((a, b) => a.path.localeCompare(b.path));
const total = rows.reduce((s, r) => s + r.size, 0), gz = rows.reduce((s, r) => s + r.gz, 0);
const kb = (n) => `${(n / 1024).toFixed(1)} KiB`;
console.log("site:", site);
for (const r of rows) console.log(`  ${r.path.padEnd(36)} ${String(r.size).padStart(8)} B  gzip ${kb(r.gz).padStart(10)}${r.used || r.path.endsWith(".wasm") || r.path.endsWith(".fwsc") || r.path.endsWith(".json") || r.path === ".nojekyll" ? "" : "  (not referenced)"}`);
console.log(`  ${rows.length} files, ${kb(total)} (gzip ${kb(gz)}); ${seen.size} pages / modules reached from index.html`);
console.log(`  package: ${pkgLine}`);
if (process.env.GITHUB_STEP_SUMMARY) {
  appendFileSync(process.env.GITHUB_STEP_SUMMARY, [
    "### Web console site", "", "| file | bytes | gzip |", "| --- | ---: | ---: |",
    ...rows.map((r) => `| \`${r.path}\` | ${r.size} | ${r.gz} |`),
    `| **${rows.length} files** | **${total}** | **${gz}** |`, "",
    `Package: ${pkgLine}`, "", errors.length ? `**${errors.length} missing references**` : "All references resolve.", "",
  ].join("\n"));
}
for (const e of errors) console.log("  MISSING", e);
console.log(errors.length ? `${errors.length} FAILED` : "all references resolve");
process.exit(errors.length ? 1 : 0);
