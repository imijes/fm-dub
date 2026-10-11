// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 installer of the web console (Phase 7.1): build/felucca.fwsc to the FM-1 over WebMIDI SysEx, with SLOOP's
// own protocol scripts (web/fm1ota.js, web/fm1pkg.js, unchanged) driven the way Hortator's installer page
// (DEADACTIVE/hortator, web/index_pkg.html) drives them:
//   - the package is offered only when it is whole: its identity (fm1pkg.js productOf, a SLOOP / Felucca "FM-1_9XX")
//     and its SHA-256 against the digest pinned in pkg.json (pkg_manifest.mjs writes it next to the build), as
//     Hortator's make_site.py pins meta.sha256: a stale or damaged download is never written;
//   - one button: an FM-1 already in update mode is resumed (the write finished), else installed from the start;
//   - after a resume, the FM-1 must come back with the package's identity (Hortator's resume(image, step, product)).
// SLOOP's fm1ota.js is newer than Hortator's copy: its resume() never writes another firmware's update loader
// ("foreign"). Passing it { product } would turn that guard off (that path is the return to the official
// firmware), so the identity check after a resume is done here, with the same waits Hortator's has.
// No DOM: app.js gives it fetch, WebMIDI and the step callbacks; tools/web_emu/web_smoke.mjs runs it on stubs.
import { Updater } from "../../web/fm1ota.js";
import { productOf, logicalImage } from "../../web/fm1pkg.js";

export const PRODUCT_RE = /^FM-1_9\d\d$/;                 // the identities of SLOOP / Felucca packages
export const STEPS_TOTAL = 74 + 1179;                      // reads: step 1 (verify), step 2 (write), as index_pkg.html
const IS_OTA = (id) => /^ota-/i.test(id.model);

// the errors' codes: fm1ota.js's (notfound, model, stopped, noloader, lost, noreturn, mismatch, badreq, foreign)
// and the package's own (http, product, sha, nopin, nopkg)
export function fail(code, msg, detail = "") { const e = new Error(msg); e.code = code; e.detail = detail; return e; }

export async function sha256hex(bytes, subtle = globalThis.crypto && globalThis.crypto.subtle) {
  if (!subtle) throw fail("sha", "this page needs https or localhost (no crypto.subtle to check the package)");
  return [...new Uint8Array(await subtle.digest("SHA-256", bytes))].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// the pin, then the package -> { product, size, sha256, image, built } (image: what the FM-1 reads, fm1pkg.js).
// The pin comes first: a deployment without a firmware package says so in pkg.json ({ available: false, reason })
// and the package is never asked for (no 404 on the public site)
export async function loadPackage(fetchFn, pkgUrl, pinUrl) {
  const p = await fetchFn(pinUrl, { cache: "no-store" });
  if (!p.ok) throw fail("nopin", `${pinUrl}: HTTP ${p.status} (pin the build: node tools/web_emu/pkg_manifest.mjs)`);
  const pin = await p.json();
  if (pin.available === false) throw fail("nopkg", pin.reason || "no firmware package in this deployment");
  const r = await fetchFn(pkgUrl, { cache: "no-store" });
  if (!r.ok) throw fail("http", `${pkgUrl}: HTTP ${r.status} (build it: ENABLE_DUB_PREAMP=1 ./build.sh)`);
  const pkg = new Uint8Array(await r.arrayBuffer());
  let product;
  try { product = productOf(pkg); } catch (e) { throw fail("product", e.message); }
  if (!PRODUCT_RE.test(product)) throw fail("product", `not a SLOOP package (${product || "no identity"})`, product);
  const sha256 = await sha256hex(pkg);
  if (pin.sha256 !== sha256 || pin.size !== pkg.length || pin.product !== product)
    throw fail("sha", `the package is not the pinned build (sha256 ${sha256.slice(0, 12)}, pinned ${String(pin.sha256).slice(0, 12)}): ` +
      "rebuild, then node tools/web_emu/pkg_manifest.mjs", sha256);
  return { product, size: pkg.length, sha256, image: logicalImage(pkg), built: pin.built || "" };
}

// the step callback's progress, 0..100 (null: no change)
export function progressOf(kind, arg) {
  if (kind === "verify") return Math.min(100, (arg / STEPS_TOTAL) * 100);
  if (kind === "write") return Math.min(100, ((74 + arg) / STEPS_TOTAL) * 100);
  if (kind === "done") return 100;
  return null;
}

// the install: "done" or "stopped" (a resume cut short: replug and press again); throws fm1ota.js's errors.
// opts: log (fm1ota.js's line log), step (kind, arg), sleep (ms -> promise), Updater (the class: tests stub it)
export async function flash(midi, pkg, opts = {}) {
  const step = opts.step || (() => {});
  const sleep = opts.sleep || ((ms) => new Promise((r) => setTimeout(r, ms)));
  const Up = opts.Updater || Updater;
  const up = new Up(midi, opts.log || (() => {}));
  const ota = await up.find(IS_OTA);
  if (!ota) {
    await up.install(pkg.image, pkg.product, step);      // step 1 (stages the loader), step 2, the reboot, the identity
    return "done";
  }
  ota.link.close();                                      // already in update mode: finish the write (SLOOP's loader only)
  if (!(await up.resume(pkg.image, step))) return "stopped";
  step("reboot");
  await sleep(3000);
  const back = await up.waitFor((id) => !IS_OTA(id), 40000);
  if (!back) throw fail("noreturn", "the FM-1 did not come back after the write: power-cycle it");
  back.link.close();
  if (back.id.text !== pkg.product) throw fail("mismatch", `written, but the FM-1 reports ${back.id.text}`, back.id.text);
  step("done", back.id.text);
  return "done";
}

// the errors as the page says them
export const ERR_TEXT = {
  notfound: "FM-1 not found: check the USB cable (a data cable), close other MIDI apps, press again.",
  model: "the connected device is not an FM-1.",
  lost: "the USB connection was lost: replug and press again.",
  noloader: "the FM-1 in update mode did not appear: replug and press again (it stays in update mode until done).",
  noreturn: "the FM-1 did not come back: power-cycle it.",
  mismatch: "written, but the FM-1 reports another version: ",
  foreign: "the FM-1 is in the update mode of another firmware: finish that update with its own updater: ",
  stopped: "stopped part way: replug and press again.",
  badreq: "the FM-1 asked for something outside the package: replug and press again.",
  http: "", product: "", sha: "", nopin: "", nopkg: "",
};
export const errorText = (e) => (e && e.code && e.code in ERR_TEXT ? (ERR_TEXT[e.code] || "") + (ERR_TEXT[e.code] ? e.detail || "" : e.message)
  : `error: ${e && e.message}`);
