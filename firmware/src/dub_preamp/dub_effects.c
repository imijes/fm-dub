/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 3.3: the input saturation, the dub siren, the space echo and the spring tank.
 *
 * In the chain (dub_mixer_intercept.c dub_chain_run):
 *   dub_sat_block      -> the big knob (3.2) -> dub_space_block -> the crossover (3.1)
 *   dub_space_block:   the siren onto the bus (stereo, panned) -> the echo and the spring take the bus (mono) at
 *                      22.05 kHz -> their return back on both sides; ECHO THROW fades the dry bus out
 *
 * SATURATION: y = tanh(x * drive) at the bus's full scale (x1 .. x8, dub_ctl.drive_q12), odd harmonics, a soft knee
 * into full scale. SLOOP's own tanh table (softclip, dsp.c: TANH_Q15, 257 points over +-2 x full scale, linear in
 * between; past that it holds tanh(2), -0.3 dBFS): no new table, the same cost as a 64-point one and four times
 * finer. Below about a third of full scale it is within 0.3 dB of the input.
 *
 * SIREN: a sine carrier (SINE, 1024 points, sine_i) swept by an LFO of three shapes (dub_ctl.siren_wave: sine,
 * square, saw), around dub_ctl.siren_note_q8, dub_ctl.siren_depth_q8 deep; pitch through pitch_inc (no pow). The
 * LFO runs at siren_rate_ch, or (siren_sync) at the division siren_div, locked to the transport's clock while it
 * plays. Gate: dub_ctl.siren_gate (the pad: MOMENTARY held, LATCH toggled by the UI), dub_live.siren_gate (MIDI),
 * or the SIREN punch slot; RETRIG mode restarts the LFO on each ev_siren. A 5 ms attack, 60 ms release (no
 * click). Auto-pan (pan_on): a ping-pong (triangle) at pan_rate_ch or pan_div, equal power from the same table.
 *
 * ECHO: a mono tape line of int16 at 22.05 kHz (DUB_ECHO_N = 1.3 s; up to DUB_ECHO_MAX_MS used) in the memory of
 * SLOOP's punch-in ring (dub_line_mem, 64 KiB of .pool: the punch-in effects are replaced in Phase 4.2 and do not
 * run in this build). In: the mono bus through DUB_FX_FIR (7.5 kHz low-pass: nothing folds back), evaluated on
 * every other sample only (cnt & 1); out: the same filter's polyphase halves back to 44.1 kHz. Everything between
 * runs once per two samples. The delay (ms, or the N_DLY division at the tempo) glides when changed: the repeats
 * bend in pitch as a tape echo's. Each repeat goes through a tone low-pass (~4.5 kHz) and a DC block, then the
 * feedback (dub_ctl.echo_fb_q12, up to 1.25: it runs away into the tanh on the write, a bounded howl), then the
 * saturation as it is written. ECHO>REV (dub_ctl.echo_to_rev): the feedback leaves the line for the spring's
 * input (crossfaded over ~1.5 ms): the repeats wash into the tank instead of coming round again.
 *   punch ECHO THROW: the dry bus fades out (~0.7 ms), the whole bus goes into the echo
 *   punch ECHO FREEZE: no input; the line goes round as it is, at exactly 1.0, no filter, whole samples (forever)
 *   punch WOW & FLUTTER: the read point wanders: a 0.7 Hz wow (+-0.9 ms) and LFSR noise flutter (low-passed)
 *
 * SPRING: Schroeder: three parallel combs (487, 617, 727 samples at 22.05 kHz: primes, 22..33 ms) with a damping
 * low-pass in each loop (dub_ctl.rev_damp_q15) and their feedback from dub_ctl.rev_decay_q15 (at most 0.93), into
 * one allpass (211 samples, 0.6). In: the mono bus x rev_send, the echo's feedback with ECHO>REV, and the crash:
 * dub_ctl / dub_live ev_crash, or the SPRING CRASH punch slot, puts a 1.5 ms burst of full-scale noise in.
 *
 * Audio ISR only. No float, no division per sample (the delay time and the LFO rates: per block). */
#ifdef ENABLE_DUB_PREAMP

/* ---------------------------------------------------------------- saturation --- */
static inline int32_t dub_sat(int32_t x)    /* tanh(x / 32768) * 32767 (held at tanh(2) past +-2 x full scale) */
{
    return softclip(x);
}

static __attribute__((noinline)) void dub_sat_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    int32_t d = dub_ctl.drive_q12;
    if (d < DUB_Q12_ONE)
        d = DUB_Q12_ONE;
    for (i = 0; i < n; i++) {                 /* (x * d >> 12: the bus is clamped first, so it fits 32 bits) */
        int32_t xl = l[i], xr = r[i];
        xl = xl > DUB_XO_CLAMP ? DUB_XO_CLAMP : xl < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xl;
        xr = xr > DUB_XO_CLAMP ? DUB_XO_CLAMP : xr < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xr;
        l[i] = dub_sat((int32_t)(((int64_t)xl * d) >> 12));
        r[i] = dub_sat((int32_t)(((int64_t)xr * d) >> 12));
    }
}

/* ---------------------------------------------------------- the 2:1 filter --- */
#define DUB_FX_TAPS 20
/* tools/gen_dub_fx.py: 20 taps, 7500 Hz, Kaiser 6.0, Q15 (sum 32768; halves 16384 / 16384) */
static const int16_t DUB_FX_FIR[DUB_FX_TAPS] = {-11, 23, 176, 230, -276, -1206, -1144, 1559, 6489, 10544, 10544, 6489, 1559, -1144, -1206, -276, 230, 176, 23, -11};

/* ------------------------------------------------------- the line's memory --- */
#define DUB_ECHO_N 28672u             /* 1.300 s at 22.05 kHz */
#define DUB_C1 487u                   /* the spring's combs and allpass, after the echo in dub_line_mem */
#define DUB_C2 617u
#define DUB_C3 727u
#define DUB_APN 211u
#define DUB_C1_AT DUB_ECHO_N
#define DUB_C2_AT (DUB_C1_AT + DUB_C1)
#define DUB_C3_AT (DUB_C2_AT + DUB_C2)
#define DUB_AP_AT (DUB_C3_AT + DUB_C3)
_Static_assert(DUB_AP_AT + DUB_APN <= DUB_LINE_N, "the echo and the spring fit the punch ring's memory");
_Static_assert((uint32_t)DUB_FS * DUB_ECHO_MAX_MS / 2000u + 64u < DUB_ECHO_N, "the echo holds its longest time");

#define DUB_ECHO_LAT (9 << 8)         /* the two FIRs' and the tick's delay, Q8 22.05 kHz samples (measured: a 250 ms
                                       * echo lands on its sample, tests/dub_fx_test.c) */
#define DUB_ECHO_TONE 18000           /* Q15 one-pole in the loop, ~4.5 kHz at 22.05 kHz */
#define DUB_RAMP 1024                 /* Q15 per step: 32 steps */
#define DUB_CRASH 32767
#define DUB_CRASH_N 32u               /* the crash: 32 ticks (1.5 ms) of noise, full scale and decaying: the tank shaken */

static struct {
    /* 44.1 kHz */
    uint32_t cnt;                     /* samples; bit 0: this one is a 22.05 kHz tick */
    int32_t dh[2 * DUB_FX_TAPS];      /* the decimator's input, oldest first from dw, written twice */
    uint32_t dw;
    int32_t ih[DUB_FX_TAPS];          /* the interpolator's 22.05 kHz input (10, written twice) */
    uint32_t iw;
    int32_t dry;                      /* the dry bus's gain, Q15 (ECHO THROW takes it to 0) */
    /* 22.05 kHz */
    uint32_t ew;                      /* echo write index */
    int32_t d_q8;                     /* the delay, Q8 samples (glides to the target) */
    int32_t tone8, dc8;               /* the loop's low-pass and its DC block: states with 8 fraction bits */
    int32_t er;                       /* ECHO>REV share, Q15 */
    uint32_t cw[3], aw;               /* the spring's indices */
    int32_t clp8[3];                  /* their damping low-passes (8 fraction bits) */
    uint32_t lfsr;                    /* flutter noise */
    int32_t flut;                     /* flutter (low-passed noise) */
    uint32_t wow_ph;                  /* wow phase (Q16 of a cycle) */
    int32_t env;                      /* the echo's level, Q15 (the scope ring) */
    uint32_t crash_c, crash_l;        /* ev_crash seen (dub_ctl, dub_live) */
    uint32_t crash_n;                 /* ticks of the crash's burst still to go */
    int8_t punch;                     /* the punch slot last block (an edge into SPRING CRASH) */
    /* the siren */
    uint32_t sph;                     /* carrier phase */
    uint32_t lph, pph;                /* LFO phase, pan phase (Q32 of a cycle) */
    int32_t senv;                     /* gate envelope, Q15 */
    uint32_t sev_c, sev_l;            /* ev_siren seen */
} dub_fx = {.dry = 32767, .d_q8 = 8000 << 8, .lfsr = 0xACE1u};

static void dub_fx_reset(void)          /* coming out of BYPASS: the filters and histories (the tape keeps its echo) */
{
    uint32_t k;
    for (k = 0; k < 2u * DUB_FX_TAPS; k++)
        dub_fx.dh[k] = 0;
    for (k = 0; k < DUB_FX_TAPS; k++)
        dub_fx.ih[k] = 0;
    dub_fx.tone8 = dub_fx.dc8 = 0;
    dub_fx.clp8[0] = dub_fx.clp8[1] = dub_fx.clp8[2] = 0;
    dub_fx.flut = 0;
}

static inline int32_t dub_s16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
/* a x g >> 15 rounded toward 0: in a feedback loop with |g| < 1 the magnitude can only shrink, so it dies to 0
 * (>> rounds toward -inf: -1 x 0.74 gives -1 again, forever: a stuck value, a DC limit cycle) */
static inline int32_t dub_mulz(int32_t a, int32_t g)
{
    int32_t v = a * g;
    return (v + (v < 0 ? 32767 : 0)) >> 15;
}
/* a one-pole low-pass in a loop: the state carries 8 fraction bits (else a step below 1 / k rounds to nothing and
 * the state sticks short of its input, which the loop then keeps alive); the output is the state rounded toward 0 */
static inline int32_t dub_lp8(int32_t *s8, int32_t x, int32_t k)
{
    *s8 += (int32_t)(((int64_t)(x * 256 - *s8) * k) >> 15);
    return (*s8 + (*s8 < 0 ? 255 : 0)) >> 8;
}

/* one 22.05 kHz input sample (the decimated mono bus) -> the echo and the spring's return */
static inline int32_t dub_space22(int32_t s, int32_t punch, int crash, int32_t dtarget_q8, int32_t fb, int32_t send,
                                  int32_t ret, int32_t rsend, int32_t rg, int32_t damp, int32_t er_to)
{
    int16_t *m = dub_line_mem;
    int32_t ein, tap, d, idx, fr, a, b, v, fbv, to_line, to_rev, sin_, sum, y, k;
    int freeze = punch == DUB_PX_ECHO_FREEZE;
    /* the delay: glide (tape) and, WOW & FLUTTER, wander */
    dub_fx.d_q8 += (dtarget_q8 - dub_fx.d_q8) >> 9;
    d = dub_fx.d_q8;
    if (punch == DUB_PX_WOW) {
        uint32_t bit = ((dub_fx.lfsr >> 0) ^ (dub_fx.lfsr >> 2) ^ (dub_fx.lfsr >> 3) ^ (dub_fx.lfsr >> 5)) & 1u;
        dub_fx.lfsr = (dub_fx.lfsr >> 1) | (bit << 15);
        dub_fx.flut += ((((int32_t)(dub_fx.lfsr & 0xFFFFu) - 32768) * 4) - dub_fx.flut) >> 6;
        dub_fx.wow_ph += 2081u;                                   /* 0.7 Hz at 22.05 kHz, Q16 */
        d += (sine_i(dub_fx.wow_ph << 16) * 20) >> 7;             /* wow: +-20 samples (Q8): +-0.9 ms */
        d += dub_fx.flut >> 7;                                    /* flutter: up to +-4 samples */
    }
    if (d < 32 << 8)
        d = 32 << 8;
    /* the tap */
    idx = d >> 8;
    fr = freeze ? 0 : d & 255;
    k = (int32_t)dub_fx.ew - idx;
    a = m[k < 0 ? k + (int32_t)DUB_ECHO_N : k];
    k--;
    b = m[k < 0 ? k + (int32_t)DUB_ECHO_N : k];
    tap = a + (((b - a) * fr) >> 8);
    /* the loop: tone, DC block, feedback; ECHO>REV takes it to the spring */
    {
        int32_t t = dub_lp8(&dub_fx.tone8, tap, DUB_ECHO_TONE);
        t -= dub_lp8(&dub_fx.dc8, t, 128);                    /* DC block: a ~14 Hz low-pass taken out */
        v = (int32_t)((int64_t)t * fb);                       /* x feedback (Q12), rounded toward 0 */
        fbv = (v + (v < 0 ? 4095 : 0)) >> 12;
    }
    if (dub_fx.er != er_to)
        dub_fx.er = dub_fx.er < er_to ? (er_to - dub_fx.er > DUB_RAMP ? dub_fx.er + DUB_RAMP : er_to)
                                      : (dub_fx.er - er_to > DUB_RAMP ? dub_fx.er - DUB_RAMP : er_to);
    to_rev = (fbv * dub_fx.er) >> 15;
    to_line = fbv - to_rev;
    ein = punch == DUB_PX_ECHO_THROW ? s : (s * send) >> 15;
    m[dub_fx.ew] = (int16_t)(freeze ? tap : dub_s16(dub_sat(ein + to_line)));
    if (++dub_fx.ew >= DUB_ECHO_N)
        dub_fx.ew = 0;
    /* the spring */
    sin_ = (((s * rsend) >> 15) + to_rev) >> 1;
    if (crash)
        dub_fx.crash_n = DUB_CRASH_N;
    if (dub_fx.crash_n) {                                     /* the burst: the flutter's LFSR, full scale, fading */
        uint32_t bit = ((dub_fx.lfsr >> 0) ^ (dub_fx.lfsr >> 2) ^ (dub_fx.lfsr >> 3) ^ (dub_fx.lfsr >> 5)) & 1u;
        dub_fx.lfsr = (dub_fx.lfsr >> 1) | (bit << 15);
        sin_ += (int32_t)(((int32_t)(dub_fx.lfsr & 0xFFFFu) - 32768) * (int32_t)dub_fx.crash_n / (int32_t)DUB_CRASH_N);
        dub_fx.crash_n--;
    }
    sum = 0;
    {
        static const uint32_t AT[3] = {DUB_C1_AT, DUB_C2_AT, DUB_C3_AT}, N[3] = {DUB_C1, DUB_C2, DUB_C3};
        uint32_t c;
        for (c = 0; c < 3u; c++) {
            int16_t *p = &m[AT[c] + dub_fx.cw[c]];
            y = *p;
            *p = (int16_t)dub_s16(sin_ + dub_mulz(dub_lp8(&dub_fx.clp8[c], y, 32767 - damp), rg));
            if (++dub_fx.cw[c] >= N[c])
                dub_fx.cw[c] = 0;
            sum += y;
        }
    }
    v = sum >> 1;                                             /* (three combs: a sum of up to 1.5 x) */
    {
        int16_t *p = &m[DUB_AP_AT + dub_fx.aw];
        int32_t bq = *p;
        y = bq - dub_mulz(v, 19661);                          /* allpass, g 0.6 */
        *p = (int16_t)dub_s16(v + dub_mulz(y, 19661));
        if (++dub_fx.aw >= DUB_APN)
            dub_fx.aw = 0;
    }
    {   /* the echo's level for the scope */
        int32_t at = tap < 0 ? -tap : tap;
        dub_fx.env = at > dub_fx.env ? at : dub_fx.env - (dub_fx.env >> 10) - (dub_fx.env > 0);   /* (to 0) */
    }
    return ((tap * ret) >> 15) + y;
}

/* the siren for this block: its LFO and pan at block rate, the carrier per sample, added to l / r */
static void dub_siren_block(int32_t *l, int32_t *r, uint32_t n, int32_t punch)
{
    uint32_t i, inc, linc, pinc, lph = dub_fx.lph;
    int32_t lfo, p16, gl = 23170, gr = 23170, lvl = dub_ctl.siren_lvl_q15, tri = 0;
    int gate = dub_ctl.siren_gate || dub_live.siren_gate || punch == DUB_PX_SIREN;
    uint32_t ec = dub_ctl.ev_siren, el = dub_live.ev_siren;
    if (ec != dub_fx.sev_c || el != dub_fx.sev_l) {          /* a re-trigger */
        dub_fx.sev_c = ec;
        dub_fx.sev_l = el;
        if (dub_ctl.siren_gate_mode == DUB_GATE_RETRIG) {
            dub_fx.lph = lph = 0;
            dub_fx.senv = 0;
        }
    }
    if (!gate && !dub_fx.senv) {
        dub_tel.siren_ph = (uint16_t)(lph >> 16);
        return;
    }
    /* the LFO: free (1/100 Hz) or on the clock (the division's length at the tempo) */
    if (dub_ctl.siren_sync) {
        uint32_t du = div_units((uint32_t)dub_ctl.siren_div % NDIV_SHORT), per = 0xFFFFFFFFu / du;
        linc = per * (uint32_t)song.g[G_BPM];
        if (song.playing)
            lph = (clk_pos % du) * per;                       /* locked to the beat */
    } else {
        linc = (uint32_t)dub_ctl.siren_rate_ch * 974u;        /* 2^32 / (100 x 44100) = 973.9 */
    }
    switch (dub_ctl.siren_wave) {
    case DUB_WAVE_SQUARE: lfo = lph < 0x80000000u ? 32767 : -32767; break;
    case DUB_WAVE_SAW: lfo = (int32_t)(lph >> 16) - 32768; break;
    default: lfo = sine_i(lph); break;
    }
    dub_fx.lph = lph + linc * n;
    dub_tel.siren_ph = (uint16_t)(lph >> 16);
    p16 = (dub_ctl.siren_note_q8 >> 4) + ((lfo * (dub_ctl.siren_depth_q8 >> 4)) >> 15);
    inc = pitch_inc((uint32_t)clamp(p16, 0, 2047));
    /* the pan: a triangle (ping-pong) */
    if (dub_ctl.pan_on) {
        uint32_t ph = dub_fx.pph, idx;
        if (dub_ctl.siren_sync) {
            uint32_t du = div_units((uint32_t)dub_ctl.pan_div % NDIV_SHORT) * 2u, per = 0xFFFFFFFFu / du;
            pinc = per * (uint32_t)song.g[G_BPM];             /* (one sweep L -> R per division) */
            if (song.playing)
                ph = ((clk_beat % 8u) * BEAT_U + clk_pos) % du * per;
        } else {
            pinc = (uint32_t)dub_ctl.pan_rate_ch * 974u;
        }
        tri = ph < 0x80000000u ? (int32_t)(ph >> 15) - 32768 : 32767 - (int32_t)((ph - 0x80000000u) >> 15);
        dub_fx.pph = ph + pinc * n;
        idx = (uint32_t)(tri + 32768) >> 8;                   /* 0 .. 255: a quarter of SINE */
        gl = SINE[256u - idx];
        gr = SINE[idx];
    }
    dub_tel.siren_pan = (int16_t)tri;
    for (i = 0; i < n; i++) {
        int32_t s, e = dub_fx.senv;
        if (gate)
            e = e + 149 > 32767 ? 32767 : e + 149;            /* 5 ms */
        else
            e = e > 12 ? e - 12 : 0;                          /* 60 ms */
        dub_fx.senv = e;
        dub_fx.sph += inc;
        s = (((sine_i(dub_fx.sph) * e) >> 15) * lvl) >> 15;
        l[i] += (s * gl) >> 15;
        r[i] += (s * gr) >> 15;
    }
}

/* the siren, the echo, the spring on the block, in place */
static __attribute__((noinline)) void dub_space_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    int32_t punch = dub_punch_now(), dt, fb, send, ret, rsend, rg, damp, er_to, dry_to;
    int crash = 0;
    {   /* the crash: an event, or the punch slot entered */
        uint32_t c = dub_ctl.ev_crash, lv = dub_live.ev_crash;
        if (c != dub_fx.crash_c || lv != dub_fx.crash_l || (punch == DUB_PX_SPRING_CRASH && dub_fx.punch != punch))
            crash = 1;
        dub_fx.crash_c = c;
        dub_fx.crash_l = lv;
        dub_fx.punch = (int8_t)punch;
    }
    dub_siren_block(l, r, n, punch);
    /* per block: the delay target (Q8, 22.05 kHz samples), the gains */
    if (dub_ctl.echo_sync && song.g[G_BPM] > 0)               /* Q8, 22.05 kHz samples: 44.1 kHz ones x 128 */
        dt = (int32_t)(dly_samples((uint32_t)dub_ctl.echo_div % NDIV_DLY) * 128u);
    else
        dt = (int32_t)((uint32_t)dub_ctl.echo_ms * 2205u * 256u / 100u);
    dt -= DUB_ECHO_LAT;
    dt = clamp(dt, 32 << 8, ((int32_t)DUB_ECHO_N - 64) << 8);
    fb = clamp(dub_ctl.echo_fb_q12, 0, DUB_FB_MAX);
    send = dub_ctl.echo_send_q15;
    ret = dub_ctl.echo_ret_q15;
    rsend = dub_ctl.rev_send_q15;
    rg = (dub_ctl.rev_decay_q15 * 30474) >> 15;               /* at most 0.93 */
    damp = dub_ctl.rev_damp_q15;
    er_to = dub_ctl.echo_to_rev ? 32767 : 0;
    dry_to = punch == DUB_PX_ECHO_THROW ? 0 : 32767;
    if (punch == DUB_PX_ECHO_FREEZE)
        fb = DUB_Q12_ONE;
    for (i = 0; i < n; i++) {
        int32_t mono = (int32_t)(((int64_t)l[i] + r[i]) >> 1), wet, dry = dub_fx.dry;
        uint32_t k;
        int64_t acc = 0;
        mono = mono > DUB_XO_CLAMP ? DUB_XO_CLAMP : mono < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : mono;
        dub_fx.dh[dub_fx.dw] = dub_fx.dh[dub_fx.dw + DUB_FX_TAPS] = mono;
        if (++dub_fx.dw >= DUB_FX_TAPS)
            dub_fx.dw = 0;
        if (dub_fx.cnt++ & 1u) {                              /* the 22.05 kHz tick */
            const int32_t *x = &dub_fx.dh[dub_fx.dw];       /* the last 20, oldest first */
            int32_t s22, w22;
            for (k = 0; k < DUB_FX_TAPS / 2u; k++)
                acc += (int64_t)DUB_FX_FIR[k] * (x[k] + x[DUB_FX_TAPS - 1u - k]);
            s22 = (int32_t)((acc + 16384) >> 15);
            w22 = dub_space22(s22, punch, crash, dt, fb, send, ret, rsend, rg, damp, er_to);
            crash = 0;
            dub_fx.ih[dub_fx.iw] = dub_fx.ih[dub_fx.iw + DUB_FX_TAPS / 2u] = w22;
            if (++dub_fx.iw >= DUB_FX_TAPS / 2u)
                dub_fx.iw = 0;
            acc = 0;
            for (k = 0; k < DUB_FX_TAPS / 2u; k++)            /* the even taps, newest first */
                acc += (int64_t)DUB_FX_FIR[2u * k] * dub_fx.ih[dub_fx.iw + DUB_FX_TAPS / 2u - 1u - k];
        } else {
            for (k = 0; k < DUB_FX_TAPS / 2u; k++)            /* the odd taps */
                acc += (int64_t)DUB_FX_FIR[2u * k + 1u] * dub_fx.ih[dub_fx.iw + DUB_FX_TAPS / 2u - 1u - k];
        }
        wet = (int32_t)((acc + 8192) >> 14);                  /* x 2: the zeros between the 22.05 kHz samples */
        if (dry != dry_to) {
            dry = dry < dry_to ? (dry_to - dry > DUB_RAMP ? dry + DUB_RAMP : dry_to)
                               : (dry - dry_to > DUB_RAMP ? dry - DUB_RAMP : dry_to);
            dub_fx.dry = dry;
        }
        if (dry != 32767) {
            l[i] = (int32_t)(((int64_t)l[i] * dry) >> 15);
            r[i] = (int32_t)(((int64_t)r[i] * dry) >> 15);
        }
        l[i] += wet;
        r[i] += wet;
    }
    dub_tel.echo_env = (uint16_t)(dub_fx.env > 32767 ? 32767 : dub_fx.env);
}

#endif /* ENABLE_DUB_PREAMP */
