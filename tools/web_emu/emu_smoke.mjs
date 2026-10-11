// SPDX-License-Identifier: GPL-3.0-only
// Smoke test of the browser emulator (tools/web_emu/preamp_emu.js, built by build_emu.sh) under Node: the riddim
// plays (sound, no NaN, bounded), the tower draws (the steel background), a pad kills SUB (the pad lit), a knob
// turns the big knob (the dial redrawn), the transport stops and starts.
//   node tools/web_emu/emu_smoke.mjs
import PreampEmu from "./preamp_emu.js";

let fails = 0;
const check = (what, ok) => { console.log(`${what.padEnd(70)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };

const M = await PreampEmu();
M._emu_init();
const frames = 44100 * 3, chunk = 2048;
let sum = 0, peak = 0, nan = 0;
for (let f = 0; f < frames; f += chunk) {
  const p = M._emu_render(chunk) >> 2;
  const a = M.HEAPF32.subarray(p, p + 2 * chunk);
  for (const v of a) { if (Number.isNaN(v)) nan++; sum += v * v; peak = Math.max(peak, Math.abs(v)); }
  if (f % (chunk * 2) === 0) M._emu_frame(46, 0);
}
const rms = Math.sqrt(sum / (2 * frames));
console.log(`  3 s of the riddim: rms ${rms.toFixed(4)}, peak ${peak.toFixed(3)}, NaN ${nan}`);
check("the riddim plays (rms > 0.01), bounded (peak <= 1.0), no NaN", rms > 0.01 && peak <= 1.0 && nan === 0);
check("the transport is playing", M._emu_playing() === 1);

const px = (ptr, x, y) => { const v = M.HEAPU16[(ptr >> 1) + y * 240 + x]; return ((v & 0xff) << 8) | (v >> 8); };
let scr = M._emu_frame(40, 1);
check("the tower on the screen: the steel background 0x10A2", px(scr, 120, 236) === 0x10a2 && px(scr, 2, 2) === 0x10a2);
const dial = () => { const s = M._emu_frame(40, 0); const o = []; for (let y = 28; y < 44; y++) for (let x = 36; x < 52; x++) o.push(px(s, x, y)); return o.join(","); };
const d0 = dial();
M._emu_knob(0, 5);
check("KNOB 1: the big knob's dial moves", dial() !== d0);

M._emu_pad(0, 1);
M._emu_render(256);
M._emu_frame(40, 0);
check("pad 1 down: SUB killed, the pad lit", (M._emu_pads_lit() & 1) === 1);
M._emu_pad(0, 0);
M._emu_render(256);
check("pad 1 up: SUB back", (M._emu_pads_lit() & 1) === 0);
M._emu_pad(11, 1); M._emu_pad(11, 0); M._emu_render(256); M._emu_frame(40, 0);
check("pad 12 tapped: BYPASS on (the toggle through the queue)", (M._emu_pads_lit() >> 11 & 1) === 1);
M._emu_pad(11, 1); M._emu_pad(11, 0); M._emu_render(256); M._emu_frame(40, 0);

M._emu_play(0);
M._emu_render(2048);
check("STOP: the transport stops", M._emu_playing() === 0);
M._emu_play(1);
M._emu_render(2048);
check("PLAY: and starts", M._emu_playing() === 1);
console.log(fails ? `${fails} FAILED` : "all ok");
process.exit(fails ? 1 : 0);
