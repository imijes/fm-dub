// SPDX-License-Identifier: GPL-3.0-only
// The page of tools/web_emu/index.html (Phase 7.2): the FM-1 shell (skin.js) around the whole firmware in
// WebAssembly (preamp_emu.js: SLOOP's engine, its UI and menu, the dub preamp; loaded at POWER ON). As Hortator's
// simulator does it, the page only passes the hardware's raw events in (a button, a key, an encoder, the MASTER pot:
// emu.c emu_btn / emu_key / emu_enc / emu_master) and draws what comes out (the screen, the LEDs); SLOOP's own
// ui_input.c decides what each one means. No parameter is set from here. The sound goes through the audio shield
// (audio_shield.js: no AudioContext before the POWER ON click); the cabinets breathe with the preamp's band meters.
// The FM-1 installer: installer.js (SLOOP's fm1ota.js / fm1pkg.js, Hortator's handshake).
import { buildSkin, keyEvent } from "./skin.js";
import { AudioShield } from "./audio_shield.js";
import { loadPackage, flash, progressOf, errorText } from "./installer.js";

const $ = (id) => document.getElementById(id);
const PKG_URL = "../../build/felucca.fwsc", PIN_URL = "./pkg.json";
const status = (s) => { $("emu-status").textContent = s; };

/* ------------------------------------------------------------------ the shell: raw events in --- */
let M = null, proc = null;
const toEngine = (f) => { if (M) f(M); };              // (before POWER ON: there is no FM-1 to press)
const shield = new AudioShield(window.AudioContext || window.webkitAudioContext, (s) => s && status(s));
const skin = buildSkin($("shell"), {
  ctl(ctl, down) {
    const [kind, id] = ctl.split(":");
    toEngine((E) => (kind === "btn" ? E._emu_btn(+id, down ? 1 : 0) : E._emu_key(+id, down ? 1 : 0)));
  },
  enc: (role, steps) => toEngine((E) => E._emu_enc(role, steps)),
  master: (raw) => toEngine((E) => E._emu_master(raw)),
  power: powerClick,
});
skin.setPower(false);

/* ------------------------------------------------------------------ the keyboard (skin.js KEYBOARD) --- */
window.addEventListener("keydown", (e) => {
  shield.wake();
  if (e.target.tagName === "INPUT") return;
  const c = keyEvent(e);
  if (!c) return;
  e.preventDefault();
  if (c.enc !== undefined) skin.encStep(c.enc, c.step);
  else skin.press(c.ctl, "kbd");
});
window.addEventListener("keyup", (e) => { const c = keyEvent(e); if (c && c.ctl) skin.release(c.ctl, "kbd"); });
window.addEventListener("blur", () => skin.releaseAll());   // (a key held while the window lost focus: let go)
window.addEventListener("pointerdown", () => shield.wake());

/* ------------------------------------------------------------------ the cabinets --- */
// a wooden box, a horn, a mid, two 15" bass drivers: their cones swell with the preamp's band meters
function cabinet() {
  const drv = (cx, cy, r, b) => `<g><circle cx="${cx}" cy="${cy}" r="${r + 6}" fill="#1a0f06"/>
    <circle cx="${cx}" cy="${cy}" r="${r + 2}" fill="#222" stroke="#000"/>
    <circle class="cone" data-b="${b}" data-r="${r}" cx="${cx}" cy="${cy}" r="${r}" fill="url(#cone)"/>
    <circle class="cone" data-b="${b}" data-r="${r * 0.3}" cx="${cx}" cy="${cy}" r="${r * 0.3}" fill="#3a3a3c" stroke="#000"/></g>`;
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
    <rect class="horn" x="70" y="44" width="30" height="8" rx="2" fill="#3a3a3c"/>
    ${drv(85, 135, 38, 2)}
    <rect x="16" y="190" width="138" height="3" fill="#2a1608"/>
    ${drv(85, 270, 64, 1)}
    ${drv(85, 425, 64, 0)}
    <rect x="12" y="500" width="146" height="8" rx="2" fill="#120a04"/>
  </svg>`;
}
$("cab-l").innerHTML = cabinet();
$("cab-r").innerHTML = cabinet();
const cones = [...document.querySelectorAll(".cone")], horns = [...document.querySelectorAll(".horn")];
const lvl = (a) => (a < 16 ? 0 : Math.max(0, Math.min(1, (20 * Math.log10(a / 32767) + 48) / 48)));
function moveCones() {
  const b = [0, 1, 2, 3].map((i) => lvl(M._emu_band(i)));
  for (const c of cones) c.setAttribute("r", (+c.dataset.r * (1 + 0.06 * b[+c.dataset.b])).toFixed(2));
  for (const h of horns) h.setAttribute("fill", b[3] > 0.7 ? "#fabc00" : "#3a3a3c");
}

/* ------------------------------------------------------------------ the screen --- */
// Hortator's blit (web/sim/app.js rgb565): RGB565 -> RGBA in one 32-bit write a pixel, rounded exactly
// (31 -> 255, 63 -> 255). The emulator's framebuffer is byte-swapped (as the panel takes it): swapped back first.
const lcd = skin.lcd.getContext("2d");
const img = lcd.createImageData(240, 240);
const px = new Uint32Array(img.data.buffer);
function blit(fb, base) {
  for (let i = 0; i < 240 * 240; i++) {
    const v = fb[base + i], c = ((v & 0xff) << 8) | (v >> 8);
    const r = (((c >> 11) & 31) * 527 + 23) >> 6, g = (((c >> 5) & 63) * 259 + 33) >> 6, b = ((c & 31) * 527 + 23) >> 6;
    px[i] = 0xff000000 | (b << 16) | (g << 8) | r;
  }
  lcd.putImageData(img, 0, 0);
}
function draw() {
  if (M && shield.running) {                             // (loading, or powered off: the screen stands still; the
    blit(M.HEAPU16, M._emu_screen() >> 1);               // firmware's clock is the audio: emu_render runs its UI)
    skin.setLeds(M._emu_leds(), M._emu_key_leds());
    moveCones();
  }
  requestAnimationFrame(draw);
}

/* ------------------------------------------------------------------ POWER ON --- */
// The only place the audio clock starts: the shield makes and resumes the AudioContext in this click's synchronous
// part, then the engine loads (the wasm) and is connected. A second click powers off (suspended), a third on again.
async function powerClick() {
  if (skin.power.disabled) return;
  if (M) {                                               // later flips: off / on, the engine kept
    if (!shield.off) skin.releaseAll();                  // (powering off: nothing stays held)
    await shield.toggle();
    skin.setPower(!shield.off);
    status(shield.off ? "powered off" : `on: ${shield.ctx.sampleRate} Hz`);
    return;
  }
  let resumed;
  try { resumed = shield.power(navigator); }             // (synchronous: inside the gesture)
  catch (e) { status(e.message); return; }
  skin.power.disabled = true;
  status("warming up the valves...");
  try {
    const { default: PreampEmu } = await import("./preamp_emu.js");
    const E = await PreampEmu();
    E._emu_init();                                       // (main.c's boot: TRACKS, the riddim playing)
    proc = shield.ctx.createScriptProcessor(2048, 0, 2);
    proc.onaudioprocess = (e) => {
      const n = e.outputBuffer.length, p = E._emu_render(n) >> 2, a = E.HEAPF32;
      const l = e.outputBuffer.getChannelData(0), r = e.outputBuffer.getChannelData(1);
      for (let i = 0; i < n; i++) { l[i] = a[p + 2 * i]; r[i] = a[p + 2 * i + 1]; }
    };
    proc.connect(shield.ctx.destination);
    await resumed;
    M = E;
    skin.setPower(true);
    const rate = shield.ctx.sampleRate;
    status(rate === 44100 ? "on: 44100 Hz, the riddim playing" : `on, but audio runs at ${rate} Hz here: pitch and tempo are off`);
  } catch (e) {
    status(`could not start: ${e.message}`);
    skin.setPower(false);
  }
  skin.power.disabled = false;
}
requestAnimationFrame(draw);

/* ------------------------------------------------------------------ the installer --- */
let pkg = null, writing = false;
const say = (s) => { $("fl-status").textContent = s; };
const log = (s) => { const l = $("log"); l.textContent += `${s}\n`; l.scrollTop = l.scrollHeight; };
const SAY = { start: "starting", verify: "checking", loader: "switching the FM-1 to update mode",
              write: "writing: do not unplug", reboot: "restarting" };
const step = (k, a) => {
  log(`${k} ${a ?? ""}`);
  if (SAY[k]) say(SAY[k]);
  if (k === "done") say(`done: the FM-1 reports ${a}`);
  const p = progressOf(k, a);
  if (p !== null) $("fl-bar").value = p;
};
const lock = (on) => { writing = on; $("fl-go").disabled = on || !pkg; };
window.addEventListener("beforeunload", (e) => { if (writing) { e.preventDefault(); e.returnValue = "writing"; } });

(async () => {
  if (!navigator.requestMIDIAccess) { say("this browser has no WebMIDI: open the page in Chrome or Edge."); return; }
  try {
    pkg = await loadPackage(fetch, PKG_URL, PIN_URL);
    $("fl-pkg").textContent = `${pkg.product}, ${pkg.size} bytes, sha256 ${pkg.sha256.slice(0, 12)} (pinned), built ${pkg.built}`;
    say("ready: connect the FM-1, then press the button");
    lock(false);
  } catch (e) { say(`could not load the package: ${errorText(e)}`); }
})();

$("fl-go").addEventListener("click", async () => {
  if (writing || !pkg) return;
  if (!confirm(`Write ${pkg.product} (sha256 ${pkg.sha256.slice(0, 12)}) to the FM-1 now? Do not unplug it until it says done.`)) return;
  lock(true);
  $("fl-bar").value = 0;
  let midi;
  try { midi = await navigator.requestMIDIAccess({ sysex: true }); }
  catch { say("MIDI (SysEx) was not allowed: allow MIDI for this site in the address bar, then press again."); lock(false); return; }
  try {
    const r = await flash(midi, pkg, { log, step });
    if (r === "stopped") say(errorText({ code: "stopped" }));
  } catch (e) {
    say(errorText(e));
    log(String((e && e.stack) || e));
  }
  lock(false);
});
