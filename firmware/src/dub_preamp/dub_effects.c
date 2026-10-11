/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 3.3 (Phase 5: SLOOP's delay and reverb): the input saturation, the dub siren, the tape echo and
 * the spring tank.
 *
 * In the chain (dub_mixer_intercept.c):
 *   saturation -> the big knob (3.2) -> dub_space_block -> the crossover (3.1) -> the sub bloom -> [+ post] -> limiter
 *   dub_space_block:   the siren (stereo, panned) and the echo and the spring's return go to the post buffer, summed
 *                      after the kills and the sub, just before the limiter: no saturation, no kill, no sub on them
 *                      (a runaway echo cannot pump the low end); ECHO THROW fades the dry bus out
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
 * the SIREN punch slot, or the SIREN SOLO latch; RETRIG mode restarts the LFO on each ev_siren. A 5 ms attack,
 * 60 ms release (no click). Auto-pan (pan_on): a ping-pong (triangle) at pan_rate_ch or pan_div, equal power.
 *
 * ECHO: SLOOP's delay, replaced (Phase 5). A mono tape line of int16 at 22.05 kHz (DUB_ECHO_N = 2.88 s) in the memory
 * of SLOOP's delay line (dub_line_mem, 128 KiB of .pool). In: the bus (with the siren) x the preamp's echo send,
 * plus SLOOP's delay sends (the tracks' DLY, the drums' DLY: fx.c send_d), through DUB_FX_FIR (7.5 kHz low-pass:
 * nothing folds back), evaluated on every other sample only (cnt & 1); out: the same filter's polyphase halves
 * back to 44.1 kHz. Everything between runs once per two samples. SLOOP's DLY page sets it: TIME (the N_DLY
 * division at the tempo), FDBK (0 .. 125 %: up to 1.25, where it runs away into the tanh on the write, a bounded
 * howl), COLR (the loop's tone low-pass, SLOOP's corner), MIX (the return). The tape motor: a new length (TIME, or
 * the tempo moving) is reached through two poles of ~93 ms each (~400 ms), so the repeats bend down or up in pitch
 * on the way, as a tape echo's or a record catching up. Each repeat goes through the tone low-pass and a DC block,
 * then the feedback, then the saturation as it is written. ECHO>REV (dub_ctl.echo_to_rev): the feedback leaves
 * the line for the spring's input (crossfaded over ~1.5 ms): the repeats wash into the tank.
 *   punch ECHO THROW: the dry bus fades out (~0.7 ms), the whole bus goes into the echo
 *   punch ECHO FREEZE: no input; the line goes round as it is, at exactly 1.0, no filter, whole samples (forever)
 *   punch WOW & FLUTTER: the read point wanders: a 0.7 Hz wow (+-0.9 ms) and LFSR noise flutter (low-passed)
 *   punch STUTTER ROLL: the line takes the whole bus for one 1/16 note (at the tempo; 125 ms stopped), the dry
 *     still playing; then it loops that 1/16 (the line at that length, frozen) and the dry fades out: a roll of the
 *     last sixteenth for as long as the pad is held (or latched). A tempo change while it rolls moves the loop's
 *     length through the tape motor, so the roll warps with it instead of clicking at a new boundary
 *
 * SPRING: SLOOP's reverb, replaced (Phase 5). Schroeder: three parallel combs (487, 617, 727 samples at 22.05 kHz:
 * primes, 22..33 ms) with a damping low-pass in each loop (REV DAMP) and their feedback from REV SIZE (at most
 * 0.93), into one allpass (211 samples, 0.6). In: the bus x the preamp's reverb send, SLOOP's reverb sends (the
 * tracks' REV, the drums' REV: fx.c send_r), the echo's feedback with ECHO>REV, and the crash: dub_ctl / dub_live
 * ev_crash, or the SPRING CRASH punch slot, puts a 1.5 ms burst of full-scale noise in. Mono, on both sides.
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
#define DUB_ECHO_N 63488u             /* 2.879 s at 22.05 kHz (Phase 5: SLOOP's delay memory; 1/4 at 41 BPM fits) */
#define DUB_C1 487u                   /* the spring's combs and allpass, after the echo in dub_line_mem */
#define DUB_C2 617u
#define DUB_C3 727u
#define DUB_APN 211u
#define DUB_C1_AT DUB_ECHO_N
#define DUB_C2_AT (DUB_C1_AT + DUB_C1)
#define DUB_C3_AT (DUB_C2_AT + DUB_C2)
#define DUB_AP_AT (DUB_C3_AT + DUB_C3)
_Static_assert(DUB_AP_AT + DUB_APN <= DUB_LINE_N, "the echo and the spring fit SLOOP's delay memory");
_Static_assert((uint32_t)DUB_FS * DUB_ECHO_MAX_MS / 2000u + 64u < DUB_ECHO_N, "the echo holds its longest time");

#define DUB_ECHO_LAT (9 << 8)         /* the two FIRs' and the tick's delay, Q8 22.05 kHz samples (measured: a 250 ms
                                       * echo lands on its sample, tests/dub_fx_test.c) */
#define DUB_GLIDE 11                  /* the tape motor: two poles of >> 11 at 22.05 kHz (93 ms each): a new length is
                                       * reached in ~400 ms, the repeats bending in pitch on the way */
#define DUB_RAMP 1024                 /* Q15 per step: 32 steps */
#define DUB_CRASH 32767
#define DUB_CRASH_N 32u               /* the crash: 32 ticks (1.5 ms) of noise, full scale and decaying: the tank shaken */

static struct {
    /* 44.1 kHz */
    uint32_t cnt;                     /* samples; bit 0: this one is a 22.05 kHz tick */
    int32_t dh[2 * DUB_FX_TAPS];      /* the decimators' input (the echo's, the spring's), oldest first from dw, */
    int32_t dhr[2 * DUB_FX_TAPS];     /* written twice */
    uint32_t dw;
    int32_t ih[DUB_FX_TAPS];          /* the interpolator's 22.05 kHz input (10, written twice) */
    uint32_t iw;
    int32_t dry;                      /* the dry bus's gain, Q15 (ECHO THROW takes it to 0) */
    /* 22.05 kHz */
    uint32_t ew;                      /* echo write index */
    int32_t g1, g2;                   /* the tape motor: the delay's length through two poles, Q12 samples */
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
    uint8_t stut;                     /* STUTTER: 0 off, 1 taking the sixteenth, 2 looping it */
    uint32_t stut_n;                  /* ticks still to take (the loop's length: the tape motor's, from the tempo) */
    int8_t punch;                     /* the punch slot last block (an edge into SPRING CRASH) */
    /* the siren */
    uint32_t sph;                     /* carrier phase */
    uint32_t lph, pph;                /* LFO phase, pan phase (Q32 of a cycle) */
    int32_t senv;                     /* gate envelope, Q15 */
    uint32_t sev_c, sev_l;            /* ev_siren seen */
} dub_fx = {.dry = 32767, .g1 = 8000 << 12, .g2 = 8000 << 12, .lfsr = 0xACE1u};

static void dub_fx_reset(void)          /* the filters and histories (the tape keeps its echo) */
{
    uint32_t k;
    for (k = 0; k < 2u * DUB_FX_TAPS; k++)
        dub_fx.dh[k] = dub_fx.dhr[k] = 0;
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

/* the tape motor: one pole of the glide toward t (Q12), landing on it exactly (no step left short of it) */
static inline void dub_glide(int32_t *g, int32_t t)
{
    int32_t d = t - *g;
    *g = d > -2048 && d < 2048 ? t : *g + (d >> DUB_GLIDE);
}

/* one 22.05 kHz tick: e the echo's input, r the spring's (both decimated) -> the echo and the spring's return.
 * dtarget: the delay (Q12 samples); tone: the loop's low-pass (Q15); ret: the echo's return (Q15) */
static inline int32_t dub_space22(int32_t e, int32_t r, int32_t punch, int crash, int32_t dtarget, int32_t fb,
                                  int32_t tone, int32_t ret, int32_t rg, int32_t lpk, int32_t er_to)
{
    int16_t *m = dub_line_mem;
    int32_t tap, d, idx, fr, a, b, v, fbv, to_line, to_rev, sin_, sum, y, k;
    int stut = dub_fx.stut, freeze = punch == DUB_PX_ECHO_FREEZE || stut == 2;
    /* the delay: the tape motor's glide (STUTTER: its sixteenth, through the same glide when the tempo moves);
     * WOW & FLUTTER: wander */
    dub_glide(&dub_fx.g1, dtarget);
    dub_glide(&dub_fx.g2, dub_fx.g1);
    d = dub_fx.g2 >> 4;                                       /* Q8 */
    if (punch == DUB_PX_WOW && !stut) {
        uint32_t bit = ((dub_fx.lfsr >> 0) ^ (dub_fx.lfsr >> 2) ^ (dub_fx.lfsr >> 3) ^ (dub_fx.lfsr >> 5)) & 1u;
        dub_fx.lfsr = (dub_fx.lfsr >> 1) | (bit << 15);
        dub_fx.flut += ((((int32_t)(dub_fx.lfsr & 0xFFFFu) - 32768) * 4) - dub_fx.flut) >> 6;
        dub_fx.wow_ph += 2081u;                                   /* 0.7 Hz at 22.05 kHz, Q16 */
        d += (sine_i(dub_fx.wow_ph << 16) * 20) >> 7;             /* wow: +-20 samples (Q8): +-0.9 ms */
        d += dub_fx.flut >> 7;                                    /* flutter: up to +-4 samples */
    }
    if (d < 32 << 8)
        d = 32 << 8;
    /* the tap (FREEZE: whole samples, so the loop goes round unchanged; the glide lands on whole samples too) */
    idx = d >> 8;
    fr = punch == DUB_PX_ECHO_FREEZE && !stut ? 0 : d & 255;
    k = (int32_t)dub_fx.ew - idx;
    a = m[k < 0 ? k + (int32_t)DUB_ECHO_N : k];
    k--;
    b = m[k < 0 ? k + (int32_t)DUB_ECHO_N : k];
    tap = a + (((b - a) * fr) >> 8);
    /* the loop: tone (DLY COLR), DC block, feedback (DLY FDBK); ECHO>REV takes it to the spring */
    {
        int32_t t = dub_lp8(&dub_fx.tone8, tap, tone);
        t -= dub_lp8(&dub_fx.dc8, t, 128);                    /* DC block: a ~14 Hz low-pass taken out */
        v = (int32_t)((int64_t)t * fb);                       /* x feedback (Q12), rounded toward 0 */
        fbv = (v + (v < 0 ? 4095 : 0)) >> 12;
    }
    if (dub_fx.er != er_to)
        dub_fx.er = dub_fx.er < er_to ? (er_to - dub_fx.er > DUB_RAMP ? dub_fx.er + DUB_RAMP : er_to)
                                      : (dub_fx.er - er_to > DUB_RAMP ? dub_fx.er - DUB_RAMP : er_to);
    to_rev = (fbv * dub_fx.er) >> 15;
    to_line = stut ? 0 : fbv - to_rev;
    m[dub_fx.ew] = (int16_t)(freeze ? tap : dub_s16(dub_sat(e + to_line)));
    if (++dub_fx.ew >= DUB_ECHO_N)
        dub_fx.ew = 0;
    if (stut)
        ret = stut == 2 ? 32767 : 0;                          /* (taking: the tape's old echo stays silent) */
    if (stut == 1 && !--dub_fx.stut_n)
        dub_fx.stut = 2;                                      /* the sixteenth is in: loop it */
    /* the spring (REV SIZE: its loop gain, DAMP: the loop's low-pass) */
    sin_ = (r + to_rev) >> 1;
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
            *p = (int16_t)dub_s16(sin_ + dub_mulz(dub_lp8(&dub_fx.clp8[c], y, lpk), rg));
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

/* the siren for this block: its LFO and pan at block rate, the carrier per sample, added to l / r (the post buffer:
 * after the kills, so the SIREN SOLO keeps it while the music is killed) */
static void dub_siren_block(int32_t *l, int32_t *r, uint32_t n, int32_t punch)
{
    uint32_t i, inc, linc, pinc, lph = dub_fx.lph;
    int32_t lfo, p16, gl = 23170, gr = 23170, lvl = dub_ctl.siren_lvl_q15, tri = 0;
    int gate = dub_ctl.siren_gate || dub_live.siren_gate || punch == DUB_PX_SIREN || dub_live.siren_solo;
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

/* the siren, the echo, the spring, for the block. The bus (l / r, and dl / dr: its dry copy while BYPASS fades, else
 * 0) takes the THROW / STUTTER dry fade; the echo's input is the bus (with the siren) x the preamp's echo send plus
 * SLOOP's delay sends (snd_d), the spring's the same x the reverb send plus SLOOP's reverb sends (snd_r); the siren
 * (stereo) and the wet return (mono) go to post_l / post_r, summed after the kills and the sub, before the limiter.
 * SLOOP's DLY page sets the echo (TIME, FDBK up to 125 % = 1.25, COLR the loop's tone, MIX its return), its REV
 * page the spring (SIZE, DAMP); the chorus stays SLOOP's (fx.c). Always runs: BYPASS takes out the colour, not
 * SLOOP's delay and reverb */
static __attribute__((noinline)) void dub_space_block(int32_t *l, int32_t *r, int32_t *dl, int32_t *dr,
                                                      const int32_t *snd_d, const int32_t *snd_r, int32_t *post_l,
                                                      int32_t *post_r, uint32_t n)
{
    uint32_t i;
    int32_t punch = dub_punch_now(), dt, fb, tone, send, ret, rsend, rg, lpk, er_to, dry_to, k44;
    int crash = 0;
    {   /* the crash: an event, or the punch slot entered */
        uint32_t c = dub_ctl.ev_crash, lv = dub_live.ev_crash;
        if (c != dub_fx.crash_c || lv != dub_fx.crash_l || (punch == DUB_PX_SPRING_CRASH && dub_fx.punch != punch))
            crash = 1;
        dub_fx.crash_c = c;
        dub_fx.crash_l = lv;
        dub_fx.punch = (int8_t)punch;
    }
    for (i = 0; i < n; i++)
        post_l[i] = post_r[i] = 0;
    dub_siren_block(post_l, post_r, n, punch);
    /* per block: the delay (Q12, 22.05 kHz samples): the DLY page's TIME at the tempo; STUTTER: a sixteenth */
    {
        uint32_t bpm_ok = song.g[G_BPM] > 0;
        uint32_t w16 = bpm_ok ? dly_samples(2) / 2u : 2756u;   /* (N_DLY 2: 1/16, in 22.05 kHz samples) */
        w16 = w16 < 64u ? 64u : w16 > DUB_ECHO_N - 64u ? DUB_ECHO_N - 64u : w16;
        if (punch == DUB_PX_STUTTER && !dub_fx.stut) {        /* STUTTER pressed: take the next sixteenth */
            dub_fx.g1 = dub_fx.g2 = (int32_t)(w16 << 12);     /* (the loop starts at its length: no glide into it) */
            dub_fx.stut_n = w16;
            dub_fx.stut = 1;
        } else if (punch != DUB_PX_STUTTER && dub_fx.stut) {
            dub_fx.stut = 0;                                  /* let go: the echo as set again (the motor glides back) */
        }
        if (dub_fx.stut) {
            dt = (int32_t)(w16 << 12);                        /* the tempo moving: the loop's length glides with it */
        } else {
            dt = (int32_t)(bpm_ok ? dly_samples((uint32_t)song.g[G_DTIME] % NDIV_DLY) * 128u : 8000u << 8);   /* Q8 */
            dt -= DUB_ECHO_LAT;
            dt = clamp(dt, 32 << 8, ((int32_t)DUB_ECHO_N - 64) << 8) << 4;   /* Q12 */
        }
    }
    fb = clamp(song.g[G_DFDBK] * DUB_Q12_ONE / 100, 0, DUB_FB_MAX);   /* DLY FDBK: 0 .. 125 % */
    k44 = 2000 + song.g[G_DCOLOR] * 240;                      /* DLY COLR, SLOOP's tone at 44.1 kHz ... */
    tone = 2 * k44 - ((k44 * k44) >> 15);                     /* ... the same corner at 22.05 kHz: 1 - (1 - k)^2 */
    tone = clamp(tone, 1000, 32767);
    ret = song.g[G_DMIX] * 258;                               /* DLY MIX */
    rg = clamp(17000 + song.g[G_RSIZE] * 104, 0, 30474);      /* REV SIZE: the loop gain (at most 0.93) */
    lpk = clamp(32767 - song.g[G_RDAMP] * 200, 4000, 32767);  /* REV DAMP */
    send = dub_ctl.echo_send_q15;
    rsend = dub_ctl.rev_send_q15;
    er_to = dub_ctl.echo_to_rev ? 32767 : 0;
    dry_to = punch == DUB_PX_ECHO_THROW ? 0 : 32767;          /* (STUTTER: per sample, below: once it loops) */
    if (punch == DUB_PX_ECHO_FREEZE)
        fb = DUB_Q12_ONE;
    for (i = 0; i < n; i++) {
        int32_t mono = (int32_t)(((int64_t)l[i] + r[i] + post_l[i] + post_r[i]) >> 1), e, rv, wet, dry = dub_fx.dry;
        uint32_t k;
        int64_t acc = 0;
        mono = mono > DUB_XO_CLAMP ? DUB_XO_CLAMP : mono < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : mono;
        e = punch == DUB_PX_ECHO_THROW || dub_fx.stut ? mono : (int32_t)(((int64_t)mono * send) >> 15) + snd_d[i];
        rv = (int32_t)(((int64_t)mono * rsend) >> 15) + snd_r[i];
        e = e > DUB_XO_CLAMP ? DUB_XO_CLAMP : e < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : e;
        rv = rv > DUB_XO_CLAMP ? DUB_XO_CLAMP : rv < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : rv;
        dub_fx.dh[dub_fx.dw] = dub_fx.dh[dub_fx.dw + DUB_FX_TAPS] = e;
        dub_fx.dhr[dub_fx.dw] = dub_fx.dhr[dub_fx.dw + DUB_FX_TAPS] = rv;
        if (++dub_fx.dw >= DUB_FX_TAPS)
            dub_fx.dw = 0;
        if (dub_fx.cnt++ & 1u) {                              /* the 22.05 kHz tick */
            const int32_t *x = &dub_fx.dh[dub_fx.dw], *y = &dub_fx.dhr[dub_fx.dw];   /* the last 20, oldest first */
            int32_t e22, r22, w22;
            int64_t accr = 0;
            for (k = 0; k < DUB_FX_TAPS / 2u; k++) {
                acc += (int64_t)DUB_FX_FIR[k] * (x[k] + x[DUB_FX_TAPS - 1u - k]);
                accr += (int64_t)DUB_FX_FIR[k] * (y[k] + y[DUB_FX_TAPS - 1u - k]);
            }
            e22 = (int32_t)((acc + 16384) >> 15);
            r22 = (int32_t)((accr + 16384) >> 15);
            w22 = dub_space22(e22, r22, punch, crash, dt, fb, tone, ret, rg, lpk, er_to);
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
        if (dub_fx.stut == 2)
            dry_to = 0;
        if (dry != dry_to) {
            dry = dry < dry_to ? (dry_to - dry > DUB_RAMP ? dry + DUB_RAMP : dry_to)
                               : (dry - dry_to > DUB_RAMP ? dry - DUB_RAMP : dry_to);
            dub_fx.dry = dry;
        }
        if (dry != 32767) {
            l[i] = (int32_t)(((int64_t)l[i] * dry) >> 15);
            r[i] = (int32_t)(((int64_t)r[i] * dry) >> 15);
            if (dl) {
                dl[i] = (int32_t)(((int64_t)dl[i] * dry) >> 15);
                dr[i] = (int32_t)(((int64_t)dr[i] * dry) >> 15);
            }
        }
        post_l[i] += wet;
        post_r[i] += wet;
    }
    dub_tel.echo_env = (uint16_t)(dub_fx.env > 32767 ? 32767 : dub_fx.env);
}

#endif /* ENABLE_DUB_PREAMP */
