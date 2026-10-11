// SPDX-License-Identifier: GPL-3.0-only
// Node check of the web console's page modules (Phase 7.1), without a browser:
//   installer.js    the real build/felucca.fwsc against its pin (pkg.json): identity, size, SHA-256, the logical
//                   image; a stale pin, a non-SLOOP package and a missing pin refused; the handshake on a stub of
//                   fm1ota.js's Updater: install from the start, resume of an FM-1 left in update mode with the
//                   identity checked after the reboot (a wrong identity: mismatch), a resume cut short: stopped
//   audio_shield.js no AudioContext before POWER ON; the context made and resumed in the click's synchronous part;
//                   the switch again suspends, again resumes; wake() brings back only a context the browser stopped
//   skin.js         the FM-1's 27 keys (16 white: the pads / grid keys 1..16 in order, 11 black), 14 buttons in
//                   panel.c's order, every control inside the 1000 x 620 body, the screen 240 x 240 in its bezel; the
//                   keyboard: Esc HOME, G GLO, ` FX, the arrows OCT- / OCT+ and SELECT, Hortator's piano rows; a
//                   held control stays down while any source holds it
//   node tools/web_emu/web_smoke.mjs        (after ./build.sh and node tools/web_emu/pkg_manifest.mjs)
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { loadPackage, flash, progressOf, errorText } from "./installer.js";
import { AudioShield, needsResume } from "./audio_shield.js";
import { KEYS, WHITE, BLACK, BTN, B, LAYOUT, keyOfWhite, keyEvent, HeldSet } from "./skin.js";

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, "..", "..");
let fails = 0;
const check = (what, ok) => { console.log(`${what.padEnd(84)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };
const rejects = async (p, code) => { try { await p; return false; } catch (e) { return e.code === code; } };

/* ---- the package and its pin */
const bytes = readFileSync(join(root, "build", "felucca.fwsc"));
const pin = JSON.parse(readFileSync(join(here, "pkg.json"), "utf8"));
const resp = (body, ok = true) => ({ ok, status: ok ? 200 : 404, arrayBuffer: async () => body, json: async () => body });
const asked = [];
const fetchOf = (pkgBody, pinBody) => async (url) => { asked.push(url); return url.endsWith(".fwsc") ? resp(pkgBody) : pinBody ? resp(pinBody) : resp(null, false); };
const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length);
const pkg = await loadPackage(fetchOf(buf, pin), "build/felucca.fwsc", "pkg.json");
console.log(`  build/felucca.fwsc: ${pkg.product}, ${pkg.size} B, sha256 ${pkg.sha256}`);
check("package: a SLOOP identity, the pinned size and SHA-256",
  /^FM-1_9\d\d$/.test(pkg.product) && pkg.size === bytes.length && pkg.sha256 === createHash("sha256").update(bytes).digest("hex"));
check("package: the logical image is the .fwsc without its 20 marker bytes", pkg.image.length === bytes.length - 20);
check("package: a stale pin (another digest) is refused", await rejects(loadPackage(fetchOf(buf, { ...pin, sha256: "0".repeat(64) }), "a.fwsc", "p"), "sha"));
check("package: no pin is refused", await rejects(loadPackage(fetchOf(buf, null), "a.fwsc", "p"), "nopin"));
asked.length = 0;
check("package: a site without a package (pkg.json available: false): said, the .fwsc never fetched (no 404)",
  await rejects(loadPackage(fetchOf(buf, { available: false, reason: "none" }), "a.fwsc", "p"), "nopkg") &&
  asked.length === 1 && asked[0] === "p");
const other = new Uint8Array(bytes);
for (let i = 0; i < 20; i++) other[i * 0x30 + 0x2f] = 0x7d;          // no identity
check("package: a package without a SLOOP identity is refused", await rejects(loadPackage(fetchOf(other.buffer, pin), "a.fwsc", "p"), "product"));
check("package: progress 0..100 through verify / write / done",
  progressOf("verify", 0) === 0 && progressOf("write", 1179) === 100 && progressOf("done") === 100 && progressOf("loader") === null);

/* ---- the handshake on a stub Updater (the device's answers scripted) */
function stubUpdater(script) {
  const calls = [];
  const dev = (text) => ({ id: { text, model: text.split("_")[0].replace(/^ota-FM-1$/, "ota-FM-1") }, link: { close: () => calls.push(`close ${text}`) } });
  class Up {
    constructor() { calls.push("new"); }
    async find(f) { const d = script.find.map(dev).find((x) => f(x.id)); calls.push(`find ${d ? d.id.text : "-"}`); return d || null; }
    async waitFor(f) { const d = script.back ? dev(script.back) : null; calls.push(`wait ${d ? d.id.text : "-"}`); return d && f(d.id) ? d : null; }
    async install(image, product, step) { calls.push(`install ${product} ${image.length}`); step("done", product); return product; }
    async resume(image, step, opts) { calls.push(`resume ${image.length}${opts ? " opts" : ""}`); step("write", 10); return script.resumed; }
  }
  return { Up, calls };
}
const run = async (script) => {
  const s = stubUpdater(script), steps = [];
  let r, err = null;
  try { r = await flash({}, pkg, { Updater: s.Up, step: (k, a) => steps.push(`${k}${a !== undefined ? ` ${a}` : ""}`), sleep: async () => {} }); }
  catch (e) { err = e; }
  return { r, err, calls: s.calls, steps };
};
let t = await run({ find: [pkg.product] });
check("install: an FM-1 running: installed from the start (step 1, step 2, the identity)",
  t.r === "done" && t.calls.includes(`install ${pkg.product} ${pkg.image.length}`) && !t.calls.some((c) => c.startsWith("resume")));
t = await run({ find: [`ota-${pkg.product}`], resumed: true, back: pkg.product });
check("resume: an FM-1 left in update mode: the write finished, no { product } (SLOOP's foreign guard kept)",
  t.r === "done" && t.calls.includes(`resume ${pkg.image.length}`) && !t.calls.some((c) => c.includes("opts")));
check("resume: then the reboot and the identity checked (Hortator's handshake)", t.steps.join(",") === `write 10,reboot,done ${pkg.product}`);
t = await run({ find: [`ota-${pkg.product}`], resumed: true, back: "FM-1_015" });
check("resume: the FM-1 back with another identity: mismatch", t.err && t.err.code === "mismatch" && t.err.detail === "FM-1_015");
t = await run({ find: [`ota-${pkg.product}`], resumed: true, back: null });
check("resume: the FM-1 not back: noreturn", t.err && t.err.code === "noreturn");
t = await run({ find: [`ota-${pkg.product}`], resumed: false });
check("resume: cut short: stopped (no reboot wait)", t.r === "stopped" && !t.calls.some((c) => c.startsWith("wait")));
check("errors: the page's words", errorText({ code: "foreign", detail: "ota-FM-1_015" }).endsWith("ota-FM-1_015") &&
  errorText({ code: "sha", message: "stale" }) === "stale" && errorText(new Error("x")) === "error: x");

/* ---- the audio shield on a fake AudioContext */
let made = 0;
const order = [];
class FakeCtx {
  constructor(o) { made++; this.o = o; this.state = "suspended"; order.push("new"); }
  resume() { order.push("resume"); this.state = "running"; this.onstatechange?.(); return Promise.resolve(); }
  suspend() { order.push("suspend"); this.state = "suspended"; this.onstatechange?.(); return Promise.resolve(); }
}
const said = [];
const sh = new AudioShield(FakeCtx, (s) => said.push(s));
check("shield: no AudioContext before POWER ON", made === 0 && !sh.running && sh.wake() === false);
const p = sh.power({});
check("shield: POWER ON makes it at 44.1 kHz and resumes it before returning (in the gesture)",
  made === 1 && sh.ctx.o.sampleRate === 44100 && order.join(",") === "new,resume" && p instanceof Promise);
await p;
check("shield: running after POWER ON", sh.running);
await sh.power({});
check("shield: the switch again: powered off (suspended, the same context)", made === 1 && sh.off && !sh.running);
sh.ctx.state = "interrupted";
check("shield: powered off: a tap does not wake it", sh.wake() === false);
await sh.power({});
check("shield: the switch again: on", made === 1 && !sh.off && sh.running);
sh.ctx.state = "interrupted";
sh.ctx.onstatechange();
check("shield: stopped by the browser: said, and a tap wakes it", said.at(-1).startsWith("sound stopped") && sh.wake() === true && sh.running);
check("shield: needsResume: suspended / interrupted only", needsResume("suspended") && needsResume("interrupted") && !needsResume("running") && !needsResume("closed"));
let threw = false;
try { new AudioShield(undefined).power({}); } catch { threw = true; }
check("shield: no Web Audio: POWER ON says so", threw);

/* ---- the skin: geometry, the keyboard */
check("skin: 27 keys, 16 white (keys 1..16 of the grid in order), 11 black, 14 buttons in panel.c's order",
  KEYS.length === 27 && WHITE.length === 16 && BLACK.length === 11 && BTN.length === 14 &&
  WHITE.every((k, i) => keyOfWhite(i) === k.n) && B.FX === 0 && B.GLO === 5 && B.HOME === 6 && B["OCT+"] === 13);
check("skin: F3 .. G5", KEYS[0].name === "F3" && KEYS[26].name === "G5");
const inside = ([x, y, w, h]) => x >= 0 && y >= 0 && x + w <= LAYOUT.W && y + h <= LAYOUT.H;
check("skin: every control inside the body, the screen 240 x 240 in its bezel",
  Object.values(LAYOUT.ctl).every(inside) && Object.values(LAYOUT.enc).every(inside) &&
  LAYOUT.lcd[2] === 240 && LAYOUT.lcd[3] === 240 && LAYOUT.lcd[0] > LAYOUT.deco.bezel[0] &&
  LAYOUT.lcd[0] + 240 < LAYOUT.deco.bezel[0] + LAYOUT.deco.bezel[2]);
const kev = (code, type = "keydown", extra = {}) => keyEvent({ code, type, repeat: false, metaKey: false, ctrlKey: false, altKey: false, ...extra });
check("keyboard: Esc = HOME, G = GLO, ` = FX (held: press and release both)",
  kev("Escape").ctl === `btn:${B.HOME}` && kev("Escape", "keyup").down === false &&
  kev("KeyG").ctl === `btn:${B.GLO}` && kev("Backquote").ctl === `btn:${B.FX}`);
check("keyboard: ← → = OCT- / OCT+, ↑ ↓ = SELECT, PgUp PgDn = PRESETS, Space PLAY, Enter REC",
  kev("ArrowLeft").ctl === `btn:${B["OCT-"]}` && kev("ArrowRight").ctl === `btn:${B["OCT+"]}` &&
  kev("ArrowUp").enc === 0 && kev("ArrowUp").step === 1 && kev("ArrowDown").step === -1 && kev("ArrowUp", "keyup") === null &&
  kev("PageDown").enc === 2 && kev("Space").ctl === `btn:${B.PLAY}` && kev("Enter").ctl === `btn:${B.REC}`);
check("keyboard: Hortator's piano rows (Z = F3, Y = G5), autorepeat and shortcuts ignored, an encoder repeats",
  kev("KeyZ").ctl === "key:0" && kev("KeyY").ctl === "key:26" && kev("KeyZ", "keydown", { repeat: true }) === null &&
  kev("KeyZ", "keydown", { metaKey: true }) === null && kev("ArrowUp", "keydown", { repeat: true }).enc === 0);
{
  const h = new HeldSet();
  const a = h.press("btn:5", "kbd"), b2 = h.press("btn:5", "ptr"), c = h.release("btn:5", "kbd"), d = h.release("btn:5", "ptr");
  check("held: GLO held by the keyboard and the pointer: down once, up only when both let go", a && !b2 && !c && d);
}
console.log(fails ? `${fails} FAILED` : "all ok");
process.exit(fails ? 1 : 0);
