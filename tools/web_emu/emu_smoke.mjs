// SPDX-License-Identifier: GPL-3.0-only
// Smoke test of the browser FM-1 (tools/web_emu/preamp_emu.js, built by build_emu.sh) under Node, driven only as
// the page drives it: raw buttons, keys, encoders and the MASTER pot (emu_btn / emu_key / emu_enc / emu_master), the
// clock being the audio (emu_render). emu_state is read, never written.
//   boot       the riddim plays (sound, no NaN, bounded), a UI frame every 15 ms of audio, TRACKS on ROOTS DARK;
//              the LED matrix is hal/fm1_input.h's
//   menu       HOME held: the menu; SELECT to AUDIO; KNOB 1 on SPEAKER EQ: FLAT -> LOW CUT -> BASS+ (stops), OCT+
//              round to FLAT; each heard: LOW CUT takes the sub-bass down, BASS+ further, with the bass's harmonics up
//              (400 .. 1000 Hz)
//   Mix grid   GLO held: the Mix layer; key 9 MUTE 5, key 13 SOLO 5 (toggles), key 10 the CONTROL TOWER latched with
//              its LOCK badge; a page button does not close it, a HOME tap does
//   FX layer   FX + OCT-: THROW latched, again let go; FX + OCT+: pad 9 the SIREN SOLO (four bands killed), again off
//   MASTER     the pot at 0: silence; back up: sound
//   stability  a minute of play with the panel in use: the heap does not grow, the buffers stay put, no NaN
//   transport  PLAY stops and starts
//   node tools/web_emu/emu_smoke.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import PreampEmu from "./preamp_emu.js";
import { B, keyOfWhite } from "./skin.js";

const here = dirname(fileURLToPath(import.meta.url));
let fails = 0;
const check = (what, ok) => { console.log(`${what.padEnd(88)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };

const M = await PreampEmu();
const S = { PAGE: 0, HOME: 1, MENU: 2, MENU_SEL: 3, LAYER: 4, LOWCUT: 5, TOWER: 6, LOCK: 7, USB_MUTE: 8, USB_SOLO: 9,
            KILL: 10, LATCHED: 11, SIREN_SOLO: 12, MI_LOWCUT: 13, LY_MIX: 14, LY_FX: 15, MASTER: 16, TRACKS_PAGE: 17 };
const st = (k) => M._emu_state(S[k]);
const FS = 44100, CHUNK = 1024;
let nan = 0;
// ms of audio; with `mono`, the samples are kept (L + R) / 2
function run(ms, mono = null) {
  let left = Math.round((FS * ms) / 1000);
  while (left > 0) {
    const n = Math.min(CHUNK, left), p = M._emu_render(n) >> 2, a = M.HEAPF32;
    for (let i = 0; i < n; i++) {
      const l = a[p + 2 * i], r = a[p + 2 * i + 1];
      if (Number.isNaN(l) || Number.isNaN(r)) nan++;
      if (mono) mono.push((l + r) / 2);
    }
    left -= n;
  }
}
const btn = (b, down) => M._emu_btn(B[b], down ? 1 : 0);
const tap = (b) => { btn(b, 1); run(60); btn(b, 0); run(60); };
const keyTap = (w) => { M._emu_key(keyOfWhite(w), 1); run(60); M._emu_key(keyOfWhite(w), 0); run(60); };
const enc = (role, s) => { M._emu_enc(role, s); run(40); };
const EN = { SELECT: 0, ALGO: 1, PRESET: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const scr = () => { const p = M._emu_screen() >> 1; return M.HEAPU16.slice(p, p + 240 * 240); };
const px = (s, x, y) => { const v = s[y * 240 + x]; return ((v & 0xff) << 8) | (v >> 8); };   // (byte-swapped)

/* ---- boot */
M._emu_init();
const f0 = M._emu_ui_frames(), boot = [];
run(3000, boot);
const rms = Math.sqrt(boot.reduce((s, v) => s + v * v, 0) / boot.length), peak = boot.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
console.log(`  3 s of the riddim: rms ${rms.toFixed(4)}, peak ${peak.toFixed(3)}, UI frames ${M._emu_ui_frames() - f0}`);
check("boot: the riddim plays (rms > 0.01), bounded (peak <= 1.0), no NaN", rms > 0.01 && peak <= 1.0 && nan === 0);
check("boot: the main loop every 15 ms of audio (200 +- 1 frames in 3 s)", Math.abs(M._emu_ui_frames() - f0 - 200) <= 1);
check("boot: the transport playing, on the TRACKS page", M._emu_playing() === 1 && st("PAGE") === st("TRACKS_PAGE") && st("MENU") === 0);
let s0 = scr(), bg = 0;
for (const v of s0) bg += (((v & 0xff) << 8) | (v >> 8)) === 0x10a2;
check(`boot: drawn on ROOTS DARK (#121212 the background: ${bg} px)`, bg > 240 * 240 / 3);
{
  const hal = readFileSync(join(here, "../../firmware/hal/fm1_input.h"), "utf8");
  const rows = hal.slice(hal.indexOf("FM1_KEYMAP[6][FM1_NCOL] = {")).split("\n").slice(1, 7)
    .map((l) => l.replace(/\/\*.*\*\//, "").match(/-?\d+/g).map(Number));
  const km = new Int8Array(M.HEAP8.buffer, M._emu_keymap(), 66);
  check("LEDs: the matrix is hal/fm1_input.h's FM1_KEYMAP", rows.flat().every((v, i) => v === km[i]));
}

/* ---- the menu: HOME held, AUDIO, SPEAKER EQ */
const bands = (x) => {                               // the energy below ~70 Hz, and in 400 .. 1000 Hz: above BASS+'s
  const lp = (f) => 1 - Math.exp((-2 * Math.PI * f) / FS);   // own ~220 Hz cut, where its harmonics land
  const a70 = lp(70), a400 = lp(400), a1k = lp(1000);
  let l70 = 0, l400 = 0, l1k = 0, e70 = 0, eh = 0;
  for (const v of x) {
    l70 += a70 * (v - l70); l400 += a400 * (v - l400); l1k += a1k * (v - l1k);
    e70 += l70 * l70; const b = l1k - l400; eh += b * b;
  }
  return { sub: Math.sqrt(e70 / x.length), harm: Math.sqrt(eh / x.length) };
};
const listen = () => { const x = []; run(6400, x); return bands(x); };   // (two bars at 75 BPM)
btn("HOME", 1); run(900); btn("HOME", 0); run(60);
check("menu: HOME held 0.7 s: the menu opens", st("MENU") === 1);
enc(EN.SELECT, 1);                                      // (a section a pass: a detent at a time, as a hand turns it)
enc(EN.SELECT, 1);
check("menu: SELECT two detents: SCREEN -> LIGHTS -> AUDIO, the cursor on SPEAKER EQ", st("MENU_SEL") === st("MI_LOWCUT"));
const menuShot = scr();
check("menu: on the screen (the picture changed from TRACKS)", menuShot.some((v, i) => v !== s0[i]));
const flat = listen();
enc(EN.K1, 1);
check("SPEAKER EQ: KNOB 1 right: LOW CUT engaged (fx_lowcut 1)", st("LOWCUT") === 1);
const cut = listen();
enc(EN.K1, 1);
check("SPEAKER EQ: right again: BASS+ (fx_lowcut 2)", st("LOWCUT") === 2);
const bass = listen();
enc(EN.K1, 1);
check("SPEAKER EQ: right at BASS+: stays", st("LOWCUT") === 2);
console.log(`  below 70 Hz: FLAT ${flat.sub.toFixed(4)}, LOW CUT ${cut.sub.toFixed(4)}, BASS+ ${bass.sub.toFixed(4)}; ` +
  `400..1000 Hz: FLAT ${flat.harm.toFixed(4)}, LOW CUT ${cut.harm.toFixed(4)}, BASS+ ${bass.harm.toFixed(4)}`);
check("SPEAKER EQ: LOW CUT takes the sub-bass down (< 60 % of FLAT)", cut.sub < 0.6 * flat.sub);
check("SPEAKER EQ: BASS+ cuts it further (< LOW CUT) and adds the bass's harmonics (400..1000 Hz > 1.2 x LOW CUT)",
  bass.sub < cut.sub && bass.harm > 1.2 * cut.harm);
tap("OCT+");
check("SPEAKER EQ: OCT+ steps round: BASS+ -> FLAT", st("LOWCUT") === 0);
btn("HOME", 1); run(900); btn("HOME", 0); run(60);
check("menu: HOME held again: closed", st("MENU") === 0);

/* ---- the Mix grid: GLO held */
btn("GLO", 1); run(300);
check("Mix grid: GLO held: the Mix layer", st("LAYER") === st("LY_MIX"));
keyTap(8);
check("Mix grid: key 9: MUTE 5 (the computer's audio)", st("USB_MUTE") === 1);
keyTap(8);
check("Mix grid: key 9 again: heard", st("USB_MUTE") === 0);
keyTap(12);
check("Mix grid: key 13: SOLO 5", st("USB_SOLO") === 1);
keyTap(12);
check("Mix grid: key 13 again: off", st("USB_SOLO") === 0);
keyTap(9);
check("Mix grid: key 10: the CONTROL TOWER, latched", st("TOWER") === 1 && st("LOCK") === 1);
btn("GLO", 0); run(200);
let s1 = scr();
check("tower: on the screen with its LOCK badge (gold, top right)", px(s1, 230, 8) === 0xfde0 && px(s1, 120, 236) === 0x10a2);
tap("LFO"); run(100);
check("tower: latched: a page button does not close it", st("TOWER") === 1);
tap("HOME"); run(100);
check("tower: a HOME tap lets it go", st("TOWER") === 0 && st("LOCK") === 0);

/* ---- the FX layer: FX + OCT- latch, FX + OCT+ siren solo */
btn("FX", 1); run(300);
check("FX layer: FX held: the dub pads", st("LAYER") === st("LY_FX"));
tap("OCT-");
keyTap(4);
check("FX + OCT-, pad 5: THROW latched (on after its key)", st("LATCHED") === 4);
keyTap(4);
check("pad 5 again: let go", st("LATCHED") === -1);
tap("OCT+");
keyTap(8);
check("FX + OCT+, pad 9: the SIREN SOLO, the four bands killed", st("SIREN_SOLO") === 1 && st("KILL") === 15);
keyTap(8);
check("pad 9 again: released", st("SIREN_SOLO") === 0 && st("KILL") === 0);
btn("FX", 0); run(300);

/* ---- MASTER */
M._emu_master(0);
run(1000);
const quiet = [];
run(1000, quiet);
check("MASTER at 0: silence (peak < 0.001)", st("MASTER") === 0 && quiet.every((v) => Math.abs(v) < 0.001));
M._emu_master(800);
const loud = [];
run(1000, loud);
check("MASTER back up: sound", loud.some((v) => Math.abs(v) > 0.05));

/* ---- stability: a minute of play, the panel in use */
{
  const heap0 = M.HEAPU8.length, pa = M._emu_render(1024), ps = M._emu_screen(), fr0 = M._emu_ui_frames();
  let stable = true, bandOk = true, seed = 7;
  const rnd = (n) => ((seed = (seed * 1664525 + 1013904223) >>> 0) >>> 8) % n;
  nan = 0;
  for (let t = 0; t < 600; t++) {                       // 100 ms at a time: a button, a key, a knob now and then
    run(100);
    stable &&= M._emu_render(0) === pa && M._emu_screen() === ps;
    for (let b = 0; b < 4; b++) { const v = M._emu_band(b); bandOk &&= v >= 0 && v <= 32767; }
    const r = rnd(10);
    if (r === 0) { M._emu_key(rnd(27), 1); run(50); for (let k = 0; k < 27; k++) M._emu_key(k, 0); }
    else if (r === 1) M._emu_enc(EN.K1 + rnd(4), rnd(5) - 2);
    else if (r === 2) { btn("GLO", 1); run(200); btn("GLO", 0); }
  }
  console.log(`  60 s: heap ${heap0} -> ${M.HEAPU8.length} B, UI frames ${M._emu_ui_frames() - fr0}, NaN ${nan}`);
  check("stability: the heap does not grow, the audio / screen buffers stay put, no NaN", M.HEAPU8.length === heap0 && stable && nan === 0);
  check("stability: the band meters in Q15 range", bandOk);
}

/* ---- the transport: the PLAY button */
tap("PLAY"); run(200);
check("PLAY: the transport stops", M._emu_playing() === 0);
tap("PLAY"); run(200);
check("PLAY: and starts", M._emu_playing() === 1);
console.log(fails ? `${fails} FAILED` : "all ok");
process.exit(fails ? 1 : 0);
