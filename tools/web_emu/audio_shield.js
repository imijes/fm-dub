// SPDX-License-Identifier: GPL-3.0-only
// The web console's audio clock (Phase 7.1), inside the browsers' autoplay rules. Nothing is created at page load:
// the AudioContext exists only after the shell's POWER ON switch is clicked, and it is made and resumed in the
// click's own synchronous part (before any await: Safari and Chrome allow sound only there), then the emulator is
// loaded and connected. A context the browser stops later (a backgrounded tab, a phone call, a route change) is
// brought back by the next tap on the page, never by itself; the switch again powers off (suspended, not closed).
// needsResume and playbackSession are Hortator's (DEADACTIVE/hortator web/sim/audio.js, GPL-3.0).
// No DOM: the AudioContext class comes in (tests/web_smoke.mjs passes a fake one).

// should the page call resume() on a context in this state?
export function needsResume(state) { return state === "suspended" || state === "interrupted"; }

// iOS mutes Web Audio with the silent switch on unless the page asks for a playback session (Safari 16.4+);
// -> true when it could ask
export function playbackSession(nav) {
  try {
    if (!nav || !nav.audioSession) return false;
    nav.audioSession.type = "playback";
    return true;
  } catch {
    return false;
  }
}

export class AudioShield {
  // AC: the AudioContext class (window.AudioContext); onState(text): what the page should say
  constructor(AC, onState = () => {}) {
    this.AC = AC;
    this.onState = onState;
    this.ctx = null;
    this.off = false;                                   // powered off by the switch (not by the browser)
  }

  get running() { return !!this.ctx && this.ctx.state === "running" && !this.off; }

  // the POWER ON click, synchronous part: the context made and resumed inside the gesture. -> the resume's promise
  // (await it after loading the engine). Throws when the browser has no Web Audio.
  power(nav) {
    if (!this.AC) throw new Error("this browser has no Web Audio: use a current Chrome, Edge, Firefox or Safari");
    if (this.ctx) return this.toggle();
    playbackSession(nav);
    this.ctx = new this.AC({ sampleRate: 44100, latencyHint: "interactive" });
    const resumed = this.ctx.resume();                  // inside the tap: sound is allowed from here on
    this.ctx.onstatechange = () => {
      if (needsResume(this.ctx.state) && !this.off) this.onState("sound stopped by the browser: tap the page to start it again");
      else if (this.ctx.state === "running") this.onState("");
    };
    return resumed;
  }

  // the switch again: off (suspend, the engine kept) / on
  toggle() {
    this.off = !this.off;
    return this.off ? this.ctx.suspend() : this.ctx.resume();
  }

  // any tap or key on the page: a context the browser stopped comes back (not one switched off)
  wake() {
    if (!this.ctx || this.off || !needsResume(this.ctx.state)) return false;
    this.ctx.resume().catch(() => {});
    return true;
  }
}
