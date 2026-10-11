// SPDX-License-Identifier: GPL-3.0-only
// The M-VAVE FM-1 shell of the web console (Phase 7.2): the panel's geometry, its controls as data, the computer
// keyboard's map, and the DOM that puts the emulator's screen in the bezel. Every control is the hardware's own: a
// button, a key or an encoder goes to the firmware as a raw event (emu.c emu_btn / emu_key / emu_enc / emu_master)
// and SLOOP's ui_input.c decides what it means, as on the FM-1.
// The geometry, the control lists, the keyboard map and the held-state bookkeeping are Hortator's simulator
// (DEADACTIVE/hortator web/sim/layout.js, controls.js; GPL-3.0): the FM-1's own landscape layout at a 1000 x 620
// design size, scaled to fit. SLOOP's repository has no hardware artwork (assets/: its logo and screenshots), so the
// body is drawn (CSS), as Hortator draws it. No DOM in the data part: web_smoke.mjs checks it under node.

// the labels in panel.c's enum order (B_FX .. B_OCTUP, EN_SELECT .. EN_K4): the ids emu_btn / emu_enc take
export const BTN = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
export const B = Object.fromEntries(BTN.map((l, i) => [l, i]));
export const ENC = ["SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4"];

const NAMES = ["F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"];
export const KEYS = Array.from({ length: 27 }, (_, n) => {          // F3 .. G5, the FM-1's 27 keys
  const name = NAMES[n % 12], octave = 3 + Math.floor((n + 5) / 12);
  return { n, name: name + octave, black: name.includes("#") };
});
export const WHITE = KEYS.filter((k) => !k.black), BLACK = KEYS.filter((k) => k.black);
export const keyOfWhite = (i) => WHITE[i].n;                          // white key i (pad / grid key i + 1) -> key n

function keybed(x0, y0, width, wW, wH, bW, bH, gapY) {             // whites in a row, the raised keys over their gaps
  const step = width / WHITE.length, out = {};
  WHITE.forEach((k, i) => { out[`key:${k.n}`] = [x0 + i * step + (step - wW) / 2, y0 + bH + gapY, wW, wH]; });
  BLACK.forEach((k) => {
    const left = WHITE.findIndex((w) => w.n === k.n - 1);
    out[`key:${k.n}`] = [x0 + (left + 1) * step - bW / 2, y0, bW, bH];
  });
  return out;
}
const ROWS = [["FX", "SEL", "ENV", "LFO", "EDIT", "GLO"], ["HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"]];
function buttonRows(rows, x0, y0, size, step, rowStep) {
  const out = {};
  rows.forEach((r, j) => r.forEach((l, i) => { out[`btn:${B[l]}`] = [x0 + i * step, y0 + j * rowStep, size, size]; }));
  return out;
}
const knob = (cx, cy, d = 46) => [cx - d / 2, cy - d / 2, d, d];

export const LAYOUT = {                                               // the FM-1 as it is (design px)
  W: 1000, H: 620,
  deco: { bezel: [270, 36, 268, 268], oct: [52, 262, 196, 64], buttons: [572, 158, 394, 156], keys: [40, 336, 920, 262] },
  lcd: [284, 50, 240, 240],
  power: [478, -12, 56, 24],                                          // the switch on the top edge
  enc: { master: knob(95, 108), 0: knob(195, 108), 2: knob(95, 214), 1: knob(195, 214),
         3: knob(632, 108), 4: knob(731, 108), 5: knob(830, 108), 6: knob(929, 108) },
  ctl: { ...buttonRows(ROWS, 588, 174, 52, 62, 68),
         [`btn:${B["OCT-"]}`]: [66, 275, 76, 38], [`btn:${B["OCT+"]}`]: [158, 275, 76, 38],
         ...keybed(60, 350, 880, 47, 122, 44, 100, 14) },
};

// the computer keyboard (KeyboardEvent.code: the physical key, any layout) -> a control. Hortator's map (two piano
// rows: Z.. / S.. from F3, Q.. / 3.. to G5; Space PLAY, Enter REC, Escape HOME, the arrows OCT- / OCT+), plus the
// console's: G GLO (held: the Mix grid), ` FX (held: the dub pads), Up / Down the SELECT encoder (pages, the menu's
// sections), PageUp / PageDown PRESETS (the menu's rows)
const PIANO = {
  KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyF: 5, KeyV: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10, KeyM: 11,
  Comma: 12, KeyL: 13, Period: 14, Semicolon: 15, Slash: 16, Quote: 17,
  KeyQ: 18, KeyW: 19, Digit3: 20, KeyE: 21, Digit4: 22, KeyR: 23, KeyT: 24, Digit6: 25, KeyY: 26,
};
export const KEYBOARD = {
  ...Object.fromEntries(Object.entries(PIANO).map(([code, n]) => [code, { kind: "key", id: n }])),
  Space: { kind: "btn", id: B.PLAY }, Enter: { kind: "btn", id: B.REC }, Escape: { kind: "btn", id: B.HOME },
  ArrowLeft: { kind: "btn", id: B["OCT-"] }, ArrowRight: { kind: "btn", id: B["OCT+"] },
  KeyG: { kind: "btn", id: B.GLO }, Backquote: { kind: "btn", id: B.FX },
  ArrowUp: { kind: "enc", id: 0, step: 1 }, ArrowDown: { kind: "enc", id: 0, step: -1 },
  PageUp: { kind: "enc", id: 2, step: -1 }, PageDown: { kind: "enc", id: 2, step: 1 },
};
const HINT_NAME = { Space: "Space", Escape: "Esc", ArrowLeft: "←", ArrowRight: "→", Backquote: "`", Comma: ",",
                    Period: ".", Slash: "/", Semicolon: ";", Quote: "'" };
export const keyHint = Object.fromEntries(Object.entries(KEYBOARD).filter(([, c]) => c.kind !== "enc")
  .map(([code, c]) => [`${c.kind}:${c.id}`, HINT_NAME[code] || code.replace(/^Key|^Digit/, "")]));

// a keyboard event -> { ctl: "key:n" | "btn:id", down } | { enc, step } | null (unmapped, autorepeat of a hold,
// a shortcut with a modifier)
export function keyEvent(e) {
  const c = KEYBOARD[e.code], down = e.type === "keydown";
  if (!c || (down && (e.metaKey || e.ctrlKey || e.altKey))) return null;
  if (c.kind === "enc") return down ? { enc: c.id, step: c.step } : null;   // (an encoder: autorepeat turns on)
  if (e.repeat) return null;
  return { ctl: `${c.kind}:${c.id}`, down };
}

// a control is held while any source (a pointer, the keyboard, a latch) holds it (Hortator's HeldSet)
export class HeldSet {
  constructor() { this.m = new Map(); }
  press(ctl, src) {                                // -> true when the control just went down
    const s = this.m.get(ctl) ?? new Set();
    const first = s.size === 0;
    s.add(src);
    this.m.set(ctl, s);
    return first;
  }
  release(ctl, src) {                              // -> true when the control just went up
    const s = this.m.get(ctl);
    if (!s || !s.delete(src)) return false;
    if (s.size) return false;
    this.m.delete(ctl);
    return true;
  }
  holds(ctl, src) { return this.m.get(ctl)?.has(src) ?? false; }
  releaseAll() { const all = [...this.m.keys()]; this.m.clear(); return all; }
}

// ---- the DOM: the shell in `host`; out: { ctl(ctl, down) the hardware event, enc(role, steps), master(raw),
// power() } -> { lcd, power, setPower(on), setLeds(btnMask, keyMask), press(ctl, src), release(ctl, src), fit() }
export function buildSkin(host, out) {
  const doc = host.ownerDocument, el = (cls, tag = "div") => { const e = doc.createElement(tag); e.className = cls; return e; };
  const place = (e, [x, y, w, ht]) => Object.assign(e.style, { left: `${x}px`, top: `${y}px`, width: `${w}px`, height: `${ht}px` });
  const held = new HeldSet(), els = {};
  const press = (ctl, src) => { if (held.press(ctl, src)) { els[ctl]?.classList.add("down"); out.ctl(ctl, true); } };
  const release = (ctl, src) => { if (held.release(ctl, src)) { els[ctl]?.classList.remove("down"); out.ctl(ctl, false); } };

  const dev = el("device");
  dev.style.width = `${LAYOUT.W}px`;
  dev.style.height = `${LAYOUT.H}px`;
  host.append(dev);
  for (const [name, box] of Object.entries(LAYOUT.deco)) { const d = el(name === "bezel" ? "bezel" : `tray ${name === "keys" ? "keybed" : ""}`); place(d, box); dev.append(d); }
  const lcd = el("lcd", "canvas");
  lcd.width = lcd.height = 240;
  lcd.setAttribute("aria-label", "screen");
  place(lcd, LAYOUT.lcd);
  dev.append(lcd);

  const power = el("power", "button");                 // the prominent POWER switch, on the shell's top edge
  power.type = "button";
  power.setAttribute("role", "switch");
  power.setAttribute("aria-checked", "false");
  power.innerHTML = "<span>POWER ON</span>";
  place(power, [LAYOUT.power[0] - 52, LAYOUT.power[1] - 14, 160, 40]);
  power.addEventListener("click", out.power);
  dev.append(power);

  const hint = (e, ctl) => { if (!keyHint[ctl]) return; const h = el("hint", "span"); h.textContent = keyHint[ctl]; e.append(h); };
  const pointerCtl = (e, ctl) => {                      // a pointer holds it; a right click latches it (Hortator)
    e.addEventListener("pointerdown", (ev) => { if (ev.button !== 0) return; ev.preventDefault(); e.setPointerCapture?.(ev.pointerId); press(ctl, "ptr"); });
    e.addEventListener("pointerup", () => release(ctl, "ptr"));
    e.addEventListener("pointercancel", () => release(ctl, "ptr"));
    e.addEventListener("contextmenu", (ev) => {
      ev.preventDefault();
      if (held.holds(ctl, "latch")) { release(ctl, "latch"); e.classList.remove("latched"); }
      else { press(ctl, "latch"); e.classList.add("latched"); }
    });
  };
  BTN.forEach((label, id) => {
    const b = el("btn"), ctl = `btn:${id}`;
    b.setAttribute("role", "button");
    b.setAttribute("aria-label", label);
    b.innerHTML = label === "PLAY" ? '<span class="two">PLAY<br>STOP</span>' : `<span>${label}</span>`;
    hint(b, ctl);
    pointerCtl(b, ctl);
    place(b, LAYOUT.ctl[ctl]);
    dev.append(b);
    els[ctl] = b;
  });
  KEYS.forEach((k) => {
    const e = el(`key${k.black ? " black" : ""}`), ctl = `key:${k.n}`;
    e.setAttribute("role", "button");
    e.setAttribute("aria-label", k.name);
    e.innerHTML = '<span class="stripe"></span>';
    hint(e, ctl);
    pointerCtl(e, ctl);
    place(e, LAYOUT.ctl[ctl]);
    dev.append(e);
    els[ctl] = e;
  });

  const turnOf = {};
  for (const [id, box] of Object.entries(LAYOUT.enc)) {
    const isMaster = id === "master", name = isMaster ? "MASTER" : ENC[Number(id)];
    const e = el(`enc${isMaster ? " master" : ""}`);
    e.innerHTML = '<span class="skirt"></span><span class="cap"></span>';
    e.setAttribute("role", "slider");
    e.setAttribute("aria-label", name);
    place(e, box);
    dev.append(e);
    const l = el("lbl");
    l.textContent = name;
    place(l, [box[0] - 30, box[1] + box[3] + 8, box[2] + 60, 14]);
    dev.append(l);
    // an encoder: detents (15 degrees each, endless); MASTER: a pot, 0 .. 1023 over 300 degrees (starts at 800)
    let turn = isMaster ? -150 + (800 / 1023) * 300 : 0, lastY = null, acc = 0;
    const show = () => e.style.setProperty("--turn", `${turn}deg`);
    const step = (s) => {
      if (isMaster) { turn = Math.max(-150, Math.min(150, turn + 6 * s)); out.master(Math.round(((turn + 150) / 300) * 1023)); }
      else { turn += 15 * s; out.enc(Number(id), s); }
      show();
    };
    show();
    e.addEventListener("pointerdown", (ev) => { lastY = ev.clientY; acc = 0; e.setPointerCapture?.(ev.pointerId); });
    e.addEventListener("pointermove", (ev) => {
      if (lastY === null) return;
      acc += lastY - ev.clientY;
      lastY = ev.clientY;
      while (Math.abs(acc) >= 8) { step(Math.sign(acc)); acc -= 8 * Math.sign(acc); }
    });
    e.addEventListener("pointerup", () => { lastY = null; });
    e.addEventListener("wheel", (ev) => { ev.preventDefault(); step(ev.deltaY < 0 ? 1 : -1); }, { passive: false });
    turnOf[id] = step;
  }

  const fit = () => {                                   // the design size scaled to the host's width
    const s = Math.min(1, host.clientWidth / LAYOUT.W);
    dev.style.setProperty("--scale", String(s));
    host.style.height = `${LAYOUT.H * s + 30}px`;
  };
  fit();
  if (typeof ResizeObserver !== "undefined") new ResizeObserver(fit).observe(host);

  return {
    dev, lcd, power, press, release,
    encStep: (role, s) => turnOf[String(role)](s),      // (the keyboard's encoders: the cap turns too)
    releaseAll() {
      for (const ctl of held.releaseAll()) { els[ctl]?.classList.remove("down", "latched"); out.ctl(ctl, false); }
    },
    setPower(on) {
      power.setAttribute("aria-checked", String(on));
      power.classList.toggle("on", on);
      power.firstChild.textContent = on ? "POWER OFF" : "POWER ON";
      dev.classList.toggle("powered-off", !on);
    },
    setLeds(btnMask, keyMask) {                          // the firmware's LEDs (emu_leds, emu_key_leds)
      BTN.forEach((_, b) => els[`btn:${b}`].classList.toggle("lit", ((btnMask >>> b) & 1) === 1));
      KEYS.forEach((k) => els[`key:${k.n}`].classList.toggle("lit", ((keyMask >>> k.n) & 1) === 1));
    },
    fit,
  };
}
