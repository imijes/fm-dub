// SPDX-License-Identifier: GPL-3.0-only
// The page of tools/web_emu/index.html: the emulator (preamp_emu.js: SLOOP + the dub preamp in WebAssembly) on the
// canvas and through WebAudio, its pads and knobs, the speaker cabinets; and the FM-1 installer, SLOOP's own
// (web/fm1ota.js, web/fm1pkg.js, unchanged), driven exactly as web/index_pkg.html drives it.
import PreampEmu from "./preamp_emu.js";
import { Updater } from "../../web/fm1ota.js";
import { productOf, logicalImage } from "../../web/fm1pkg.js";

const $ = (id) => document.getElementById(id);
const PKG_URL = "../../build/felucca.fwsc";

/* ------------------------------------------------------------------ the pads, the knobs --- */
// pads 1..16 as the FX layer (dub_midi.c): their names, keys, kind (kill / hot: a red lens)
const PADS = [
  ["KILL S", "1", "kill"], ["KILL B", "2", "kill"], ["KILL M", "3", "kill"], ["KILL T", "4", "kill"],
  ["THROW", "5", "hot"], ["FREEZE", "6", "hot"], ["DRIFT", "7", ""], ["ROLL", "8", ""],
  ["SIREN", "Q", "hot"], ["CRASH", "W", "hot"], ["E>REV", "E", ""], ["BYPASS", "R", "hot"],
  ["CLK", "T", ""], ["BLOOM", "Y", ""], ["-", "U", ""], ["-", "I", ""],
];
const KNOBS = [["BIG KNOB", "A", "Z"], ["DRIVE", "S", "X"], ["FEEDBACK", "D", "C"], ["SIREN", "F", "V"]];
const NOTE_KEYS = { h: 69, j: 72, k: 74, l: 76, ";": 79, "'": 81 };   // track 3: A minor pentatonic

let M = null, ctx = null, proc = null;
const padEls = [], knobEls = [];
const knobAngle = [0, 0, 0, 0];

function buildPads() {
  PADS.forEach(([name, key, kind], i) => {
    const b = document.createElement("div");
    b.className = `pad ${kind}`;
    b.innerHTML = `${name}<span class="k">${key}</span>`;
    const down = (e) => { e.preventDefault(); pad(i, 1); };
    const up = () => pad(i, 0);
    b.addEventListener("pointerdown", down);
    b.addEventListener("pointerup", up);
    b.addEventListener("pointerleave", up);
    $("pads").appendChild(b);
    padEls.push(b);
  });
}
const padHeld = new Set();
function pad(i, down) {
  if (!M) return;
  if (down ? padHeld.has(i) : !padHeld.has(i)) return;
  down ? padHeld.add(i) : padHeld.delete(i);
  padEls[i].classList.toggle("down", !!down);
  M._emu_pad(i, down);
}

function knobSvg() {
  return `<svg viewBox="0 0 70 70"><circle cx="35" cy="35" r="31" fill="#1b1c1d" stroke="#000"/>
    <circle cx="35" cy="35" r="24" fill="url(#kg)" stroke="#4e5054"/>
    <defs><radialGradient id="kg" cx=".4" cy=".35"><stop offset="0" stop-color="#5a5c60"/><stop offset="1" stop-color="#1e1f20"/></radialGradient></defs>
    <line class="ptr" x1="35" y1="35" x2="35" y2="14" stroke="#fabc00" stroke-width="3" stroke-linecap="round"/></svg>`;
}
function buildKnobs() {
  KNOBS.forEach(([name, up, dn], k) => {
    const d = document.createElement("div");
    d.className = "knob";
    d.innerHTML = `${knobSvg()}<div class="lab">${name}</div><div class="key">${up} / ${dn}</div>`;
    let lastY = null, acc = 0;
    d.addEventListener("pointerdown", (e) => { lastY = e.clientY; acc = 0; d.setPointerCapture(e.pointerId); });
    d.addEventListener("pointermove", (e) => {
      if (lastY === null) return;
      acc += lastY - e.clientY;
      lastY = e.clientY;
      while (Math.abs(acc) >= 8) { turn(k, Math.sign(acc)); acc -= 8 * Math.sign(acc); }
    });
    d.addEventListener("pointerup", () => { lastY = null; });
    d.addEventListener("wheel", (e) => { e.preventDefault(); turn(k, e.deltaY < 0 ? 1 : -1); }, { passive: false });
    $("knobs").appendChild(d);
    knobEls.push(d.querySelector(".ptr"));
  });
}
function turn(k, s) {
  if (!M) return;
  M._emu_knob(k, s);
  knobAngle[k] = Math.max(-150, Math.min(150, knobAngle[k] + s * 15));
  knobEls[k].setAttribute("transform", `rotate(${knobAngle[k]} 35 35)`);
}

const padKey = (key) => {
  const k = key.toUpperCase();
  return PADS.findIndex(([, pk]) => pk === k);
};
window.addEventListener("keydown", (e) => {
  if (e.repeat || e.target.tagName === "INPUT") return;
  const i = padKey(e.key);
  if (i >= 0) { pad(i, 1); return; }
  const k = KNOBS.findIndex(([, u, d]) => u === e.key.toUpperCase() || d === e.key.toUpperCase());
  if (k >= 0) { turn(k, KNOBS[k][1] === e.key.toUpperCase() ? 1 : -1); return; }
  if (e.key in NOTE_KEYS && M) { M._emu_note(NOTE_KEYS[e.key], 1); return; }
  if (e.key === " " && M) { e.preventDefault(); togglePlay(); }
});
window.addEventListener("keyup", (e) => {
  const i = padKey(e.key);
  if (i >= 0) pad(i, 0);
  if (e.key in NOTE_KEYS && M) M._emu_note(NOTE_KEYS[e.key], 0);
});

/* ------------------------------------------------------------------ the cabinets --- */
// a wooden box, a horn, a mid, two 15" bass drivers: their cones swell with the preamp's band meters
function cabinet() {
  const drv = (cx, cy, r, b) => `<g><circle cx="${cx}" cy="${cy}" r="${r + 6}" fill="#1a0f06"/>
    <circle cx="${cx}" cy="${cy}" r="${r + 2}" fill="#222" stroke="#000"/>
    <circle class="cone" data-b="${b}" data-r="${r}" cx="${cx}" cy="${cy}" r="${r}" fill="url(#cone)"/>
    <circle class="cap" data-b="${b}" data-r="${r * 0.3}" cx="${cx}" cy="${cy}" r="${r * 0.3}" fill="#3a3a3c" stroke="#000"/></g>`;
  return `<svg viewBox="0 0 170 520">
    <defs>
      <linearGradient id="wood" x1="0" x2="1"><stop offset="0" stop-color="#4a2a12"/><stop offset=".15" stop-color="#8a5a2e"/>
        <stop offset=".5" stop-color="#6b3f1f"/><stop offset=".85" stop-color="#8a5a2e"/><stop offset="1" stop-color="#4a2a12"/></linearGradient>
      <radialGradient id="cone" cx=".45" cy=".4"><stop offset="0" stop-color="#555"/><stop offset=".7" stop-color="#262626"/>
        <stop offset="1" stop-color="#111"/></radialGradient>
      <pattern id="grain" width="170" height="14" patternUnits="userSpaceOnUse"><path d="M0 7 Q40 3 85 7 T170 7" stroke="rgba(0,0,0,.18)" fill="none"/></pattern>
    </defs>
    <rect x="4" y="4" width="162" height="512" rx="6" fill="url(#wood)" stroke="#2a1608" stroke-width="3"/>
    <rect x="4" y="4" width="162" height="512" rx="6" fill="url(#grain)"/>
    <rect x="18" y="20" width="134" height="56" rx="4" fill="#120a04"/>
    <path d="M30 30 L140 30 L118 66 L52 66 Z" fill="#1d1d1e" stroke="#000"/>
    <rect class="horn" data-b="3" x="70" y="44" width="30" height="8" rx="2" fill="#3a3a3c"/>
    ${drv(85, 135, 38, 2)}
    <rect x="16" y="190" width="138" height="3" fill="#2a1608"/>
    ${drv(85, 270, 64, 1)}
    ${drv(85, 425, 64, 0)}
    <rect x="12" y="500" width="146" height="8" rx="2" fill="#120a04"/>
  </svg>`;
}
let cones = [];
function buildCabinets() {
  $("cab-l").innerHTML = cabinet();
  $("cab-r").innerHTML = cabinet();
  cones = [...document.querySelectorAll(".cone, .cap")];
}
const lvl = (a) => (a < 16 ? 0 : Math.max(0, Math.min(1, (20 * Math.log10(a / 32767) + 48) / 48)));
function moveCones() {
  const b = [0, 1, 2, 3].map((i) => lvl(M._emu_band(i)));
  for (const c of cones) {
    const v = b[+c.dataset.b], r = +c.dataset.r;
    c.setAttribute("r", (r * (1 + 0.06 * v)).toFixed(2));
  }
  for (const h of document.querySelectorAll(".horn"))
    h.setAttribute("fill", b[3] > 0.7 ? "#fabc00" : "#3a3a3c");
}

/* ------------------------------------------------------------------ the emulator --- */
const lcd = $("lcd").getContext("2d");
const img = lcd.createImageData(240, 240);
let lastT = 0;
function draw(t) {
  const ms = lastT ? Math.min(100, Math.round(t - lastT)) : 16;
  lastT = t;
  const p = M._emu_frame(ms, 0) >> 1, s = M.HEAPU16, d = img.data;
  for (let i = 0; i < 240 * 240; i++) {
    const v = s[p + i], c = ((v & 0xff) << 8) | (v >> 8);
    d[4 * i] = ((c >> 11) * 255) / 31;
    d[4 * i + 1] = (((c >> 5) & 63) * 255) / 63;
    d[4 * i + 2] = ((c & 31) * 255) / 31;
    d[4 * i + 3] = 255;
  }
  lcd.putImageData(img, 0, 0);
  const lit = M._emu_pads_lit();
  padEls.forEach((el, i) => el.classList.toggle("lit", !!((lit >> i) & 1)));
  $("play").textContent = M._emu_playing() ? "STOP" : "PLAY";
  moveCones();
  requestAnimationFrame(draw);
}
function togglePlay() { if (M) M._emu_play(M._emu_playing() ? 0 : 1); }

$("power").addEventListener("click", async () => {
  if (M) return;
  $("power").disabled = true;
  $("emu-status").textContent = "warming up the valves...";
  try {
    M = await PreampEmu();
    M._emu_init();
    ctx = new AudioContext({ sampleRate: 44100 });
    proc = ctx.createScriptProcessor(2048, 0, 2);
    proc.onaudioprocess = (e) => {
      const n = e.outputBuffer.length, p = M._emu_render(n) >> 2, a = M.HEAPF32;
      const l = e.outputBuffer.getChannelData(0), r = e.outputBuffer.getChannelData(1);
      for (let i = 0; i < n; i++) { l[i] = a[p + 2 * i]; r[i] = a[p + 2 * i + 1]; }
    };
    proc.connect(ctx.destination);
    await ctx.resume();
    $("play").disabled = false;
    $("emu-status").textContent = `on: ${ctx.sampleRate} Hz, the riddim playing`;
    requestAnimationFrame(draw);
  } catch (e) {
    $("emu-status").textContent = `could not start: ${e.message}`;
    $("power").disabled = false;
    M = null;
  }
});
$("play").addEventListener("click", togglePlay);

/* ------------------------------------------------------------------ the installer --- */
// SLOOP's (web/fm1ota.js): exactly the steps of web/index_pkg.html's Install button
let image = null, product = "", writing = false;
const say = (s) => { $("fl-status").textContent = s; };
const log = (s) => { const l = $("log"); l.textContent += `${s}\n`; l.scrollTop = l.scrollHeight; };
const total = 74 + 1179;                                     // reads: step 1, step 2 (as index_pkg.html)
const step = (k, a) => {
  log(`${k} ${a ?? ""}`);
  if (k === "start") say("starting");
  if (k === "verify") { say("checking"); $("fl-bar").value = Math.min(100, (a / total) * 100); }
  if (k === "loader") say("switching the FM-1 to update mode");
  if (k === "write") { say("writing: do not unplug"); $("fl-bar").value = Math.min(100, ((74 + a) / total) * 100); }
  if (k === "reboot") say("restarting");
  if (k === "done") { $("fl-bar").value = 100; say(`done: the FM-1 reports ${a}`); }
};
const ERR = {
  notfound: "FM-1 not found: check the USB cable (a data cable), close other MIDI apps, press again.",
  model: "the connected device is not an FM-1.",
  lost: "the USB connection was lost: replug and press again.",
  noloader: "the FM-1 in update mode did not appear: replug and press again (it stays in update mode until done).",
  noreturn: "the FM-1 did not come back: power-cycle it.",
  mismatch: "written, but the FM-1 reports another version: ",
  foreign: "the FM-1 is in the update mode of another firmware: finish that update with its own updater: ",
  stopped: "stopped part way: replug and press again.",
};
const lock = (on) => { writing = on; $("fl-go").disabled = on || !image; };
window.addEventListener("beforeunload", (e) => { if (writing) { e.preventDefault(); e.returnValue = "writing"; } });

(async () => {
  if (!navigator.requestMIDIAccess) { say("this browser has no WebMIDI: open the page in Chrome or Edge."); return; }
  try {
    const r = await fetch(PKG_URL, { cache: "no-store" });
    if (!r.ok) throw new Error(`${PKG_URL}: HTTP ${r.status} (build it: ENABLE_DUB_PREAMP=1 ./build.sh)`);
    const pkg = new Uint8Array(await r.arrayBuffer());
    product = productOf(pkg);
    if (!/^FM-1_9\d\d$/.test(product)) { say(`not a SLOOP package (${product || "no identity"})`); return; }
    const h = [...new Uint8Array(await crypto.subtle.digest("SHA-256", pkg))].map((b) => b.toString(16).padStart(2, "0")).join("");
    image = logicalImage(pkg);
    $("fl-pkg").textContent = `${product}, ${pkg.length} bytes, sha256 ${h.slice(0, 12)}, ${r.headers.get("last-modified") || ""}`;
    say("ready: connect the FM-1, then press the button");
    lock(false);
  } catch (e) { say(`could not load the package: ${e.message}`); }
})();

$("fl-go").addEventListener("click", async () => {
  if (writing || !image) return;
  if (!confirm("Write build/felucca.fwsc to the FM-1 now? Do not unplug it until it says done.")) return;
  lock(true);
  $("fl-bar").value = 0;
  let midi;
  try { midi = await navigator.requestMIDIAccess({ sysex: true }); }
  catch (e) { say("MIDI (SysEx) was not allowed: allow MIDI for this site in the address bar, then press again."); lock(false); return; }
  try {
    const up = new Updater(midi, log);
    const ota = await up.find((id) => /^ota-/i.test(id.model));
    if (ota) {                                                 // already in update mode: finish it
      ota.link.close();
      say((await up.resume(image, step)) ? "done" : ERR.stopped);
    } else {
      await up.install(image, product, step);
    }
  } catch (e) {
    say(e && e.code && ERR[e.code] ? ERR[e.code] + (e.detail || "") : `error: ${e && e.message}`);
    log(String((e && e.stack) || e));
  }
  lock(false);
});

buildPads();
buildKnobs();
buildCabinets();
