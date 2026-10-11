/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 3.4: the sub bloom and the safety limiter, the end of the chain (dub_mixer_intercept.c):
 *   ... -> the crossover (3.1) -> dub_sub_block -> dub_lim_block -> the bus back to SLOOP's master
 *
 * SUB BLOOM: a sine an octave under the low end, added to both sides.
 *   Arming (dub_ctl.sub_arm, pad 14 SUB BLOOM, CC 53): while the transport plays, it comes in (and goes out) on the
 *   next downbeat, the first sample of a bar (a beat with clk_beat % 4 == 0, as the metronome's accent), found to
 *   the sample in the block that crosses it; stopped, at once. It comes in with its phase at exactly 0.
 *   Tracking: the crossover's SUB band (LR4 below 80 Hz, mono: dub_xo.sub), its rising zero crossings through a
 *   Schmitt trigger (hysteresis an eighth of its level), the interval between two accepted when it is a 20..80 Hz
 *   period (551..2205 samples), smoothed; the oscillator runs at half that frequency and is pulled gently toward
 *   the phase that keeps it locked to the input (0 or 180 degrees at the input's crossing: a sub octave crosses
 *   once for every two). Level: the SUB band's envelope (instant attack, ~46 ms release) x dub_ctl.sub_lvl_q15,
 *   so it swells and decays with the bass, x the SUB kill (a killed SUB band takes the bloom with it).
 *   Silence gate: below DUB_SUB_GATE (~-50 dBFS) the tracker freezes (no crossing counted: hiss is not a bass)
 *   and the oscillator fades out.
 *   Fades: never a snap. Every change of level (engaging, disengaging, the gate, no period yet) ramps over 32
 *   samples (0.73 ms); a fade out starts at the oscillator's next zero crossing, so the ramp begins near 0.
 *   Idle (not armed, not sounding): the function returns before any work, and the crossover stops keeping SUB.
 *
 * LIMITER: a peak limiter with an 8-sample look-ahead, ceiling dub_ctl.out_q15 (the bus's Q15: 32767 = SLOOP's
 * master full scale, before the MASTER knob and SLOOP's own limiter). Per sample: the peak of |L|, |R| and the
 * delay; per group of 8: the gain. The group's peak raises the envelope at once, which falls back toward the
 * ceiling by >> DUB_RELEASE_SHIFT a group (~90 ms: a long overload holds the gain down, a short one lets go
 * sooner); the gain for the group = ceiling / envelope (one division per 8 samples); while the group waits in
 * the delay the gain ramps down to it linearly (no step: a smooth duck, not a click), and it never rises above
 * what the samples being output need. So nothing leaves above the ceiling (a final clamp makes sure). Gain 1.0
 * passes the delayed samples bit for bit. dub_tel.lim_gr_q15: the gain now (32767 none), lim_hits: blocks that
 * reduced. The look-ahead delays the chain by 8 samples (0.18 ms).
 *
 * Audio ISR only. No float. */
#ifdef ENABLE_DUB_PREAMP

/* ---------------------------------------------------------------- sub bloom --- */
#define DUB_SUB_GATE 100              /* Q15 envelope: below it, silence (~-50 dBFS) */
#define DUB_SUB_PMIN 500u             /* periods accepted: 88 .. 18 Hz (the band's 20..80 Hz and a margin) */
#define DUB_SUB_PMAX 2450u
#define DUB_SUB_FADE 1024             /* Q15 per sample: 32 samples */

static struct {
    uint8_t on;                       /* engaged (it came in on a downbeat) */
    uint8_t neg;                      /* the Schmitt trigger: the input went below -h */
    uint8_t falling;                  /* a fade out under way (it waited for a zero crossing) */
    uint8_t was_playing;
    uint8_t edge;                     /* a downbeat falls on the next block's first sample */
    uint32_t last_beat;               /* clk_beat at the last block */
    uint32_t since;                   /* samples since the last rising crossing */
    uint32_t per;                     /* the input's period, samples (0: none yet) */
    uint32_t ph, inc;                 /* the oscillator (Q32 of a cycle) */
    int32_t env8;                     /* the SUB band's level, Q15 with 8 fraction bits (the release reaches 0) */
    int32_t env;                      /* ... as Q15 */
    int32_t g;                        /* the fade, Q15 */
    int32_t last_s;                   /* the oscillator's last value (its zero crossings) */
} dub_sub;

/* the sample of this block where a downbeat falls, or -1. The clock is already at the next block's start (seq.c
 * events_block runs first): a beat crossed in this block shows as clk_beat moved on, clk_pos units past it */
static int32_t dub_sub_downbeat(uint32_t n)
{
    uint32_t beat = clk_beat, pos = clk_pos, u = song.g[G_BPM] > 0 ? (uint32_t)song.g[G_BPM] : 1u, back;
    int32_t at = -1;
    if (!song.playing) {
        dub_sub.was_playing = 0;
        dub_sub.edge = 0;
        return -1;
    }
    if (dub_sub.edge) {                                 /* the last block ended exactly on it */
        dub_sub.edge = 0;
        at = 0;
    }
    if (!dub_sub.was_playing) {                         /* PLAY: the first block starts on beat 0, a downbeat */
        dub_sub.was_playing = 1;
        dub_sub.last_beat = beat;
        return beat == 0u ? 0 : -1;
    }
    if (beat != dub_sub.last_beat && beat % 4u == 0u) {
        back = (pos + u - 1u) / u;                      /* samples from the downbeat to the block's end */
        if (!back)
            dub_sub.edge = 1;                           /* exactly at the end: the next block's sample 0 */
        else
            at = back >= n ? 0 : (int32_t)(n - back);
    }
    dub_sub.last_beat = beat;
    return at;
}

static void dub_sub_reset(void)       /* coming out of BYPASS: re-armed, it comes in on the next downbeat */
{
    uint8_t *p = (uint8_t *)&dub_sub;
    uint32_t k;
    for (k = 0; k < sizeof dub_sub; k++)
        p[k] = 0;
    dub_sub.last_beat = clk_beat;
    dub_sub.was_playing = (uint8_t)(song.playing != 0);
}

static __attribute__((noinline)) void dub_sub_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, want = dub_ctl.sub_arm, playing = song.playing;
    int32_t at, lvl, kill_g;
    const int32_t *x = dub_xo.sub;
    if (!want && !dub_sub.on && !dub_sub.g) {           /* idle: nothing at all */
        dub_xo.want_sub = 0;
        dub_sub.was_playing = (uint8_t)(playing != 0);
        dub_sub.last_beat = clk_beat;
        dub_tel.sub_on = 0;
        return;
    }
    dub_xo.want_sub = 1;
    at = dub_sub_downbeat(n);
    if (!playing && want != dub_sub.on)
        at = 0;                                         /* stopped: no bar to wait for */
    lvl = dub_ctl.sub_lvl_q15;
    kill_g = dub_xo.g[DUB_SUB];
    for (i = 0; i < n && i < CTL; i++) {
        int32_t v = x[i], a = v < 0 ? -v : v, h, s, to, out;
        if (a > 32767)
            a = 32767;                                  /* (the bus may run past full scale: keep s x env in 32 bits) */
        /* the level, the gate */
        dub_sub.env8 = a * 256 > dub_sub.env8 ? a * 256 : dub_sub.env8 - (dub_sub.env8 >> 11);   /* (Q8: >> 11 of
                                                         * a plain Q15 stops below 2048 and the gate never shuts) */
        dub_sub.env = dub_sub.env8 >> 8;
        if (dub_sub.env > DUB_SUB_GATE) {               /* tracking (frozen in silence) */
            h = dub_sub.env >> 3;
            if (v < -h)
                dub_sub.neg = 1;
            else if (dub_sub.neg && v > h) {            /* a rising crossing */
                dub_sub.neg = 0;
                if (dub_sub.since >= DUB_SUB_PMIN && dub_sub.since <= DUB_SUB_PMAX) {
                    int32_t e0, e1;
                    uint32_t m0, m1;
                    dub_sub.per = dub_sub.per ? (dub_sub.per * 3u + dub_sub.since) >> 2 : dub_sub.since;
                    dub_sub.inc = 0x80000000u / dub_sub.per;   /* half the input's frequency */
                    e0 = (int32_t)(0u - dub_sub.ph);            /* the lock: toward 0 or 180 degrees */
                    e1 = (int32_t)(0x80000000u - dub_sub.ph);
                    m0 = e0 < 0 ? 0u - (uint32_t)e0 : (uint32_t)e0;   /* (unsigned: |-2^31| fits) */
                    m1 = e1 < 0 ? 0u - (uint32_t)e1 : (uint32_t)e1;
                    dub_sub.ph += (uint32_t)((m0 < m1 ? e0 : e1) >> 4);
                }
                dub_sub.since = 0;
            }
        }
        if (dub_sub.since < 0xFFFFu)
            dub_sub.since++;
        /* engage / disengage on the downbeat */
        if ((int32_t)i == at && want != dub_sub.on) {
            dub_sub.on = (uint8_t)want;
            if (want) {
                dub_sub.ph = 0;                         /* in on the downbeat, at 0 degrees */
                dub_sub.last_s = 0;
                dub_sub.falling = 0;
            }
        }
        /* the oscillator and its fade */
        if (dub_sub.per)
            dub_sub.ph += dub_sub.inc;
        s = sine_i(dub_sub.ph);
        to = dub_sub.on && dub_sub.per && dub_sub.env > DUB_SUB_GATE ? 32767 : 0;
        if (to > dub_sub.g) {
            dub_sub.falling = 0;
            dub_sub.g = dub_sub.g + DUB_SUB_FADE > to ? to : dub_sub.g + DUB_SUB_FADE;
        } else if (to < dub_sub.g) {
            if (!dub_sub.falling && ((s < 0) != (dub_sub.last_s < 0) || !dub_sub.per))
                dub_sub.falling = 1;                    /* the fade out from a zero crossing */
            if (dub_sub.falling)
                dub_sub.g = dub_sub.g - DUB_SUB_FADE < 0 ? 0 : dub_sub.g - DUB_SUB_FADE;
        }
        dub_sub.last_s = s;
        if (dub_sub.g) {
            out = (s * dub_sub.env) >> 15;
            out = (out * lvl) >> 15;
            out = (int32_t)(((int64_t)out * dub_sub.g * kill_g) >> 30);
            l[i] += out;
            r[i] += out;
        }
    }
    dub_tel.sub_on = (uint8_t)(dub_sub.on && dub_sub.g);
}

/* ------------------------------------------------------------------ limiter --- */
#define DUB_LIM_LA 8u                 /* look-ahead and group, samples (a power of two) */
#define DUB_LIM_SHIFT 3               /* log2 of it: the ramp's step is a shift, not a division */
_Static_assert(DUB_LIM_LA == 1u << DUB_LIM_SHIFT, "the group is 2^DUB_LIM_SHIFT");
#define DUB_RELEASE_SHIFT 9           /* the envelope falls back to the ceiling by >> 9 a group: ~90 ms */

static struct {
    int32_t dl[DUB_LIM_LA], dr[DUB_LIM_LA];   /* the delay */
    uint32_t k;                       /* the position in the group (0 .. 7) */
    int32_t env;                      /* the peak envelope, Q15 (>= the ceiling when reducing) */
    int32_t g;                        /* the gain now, Q16 (65536 = 1.0: the ramp needs the fraction) */
    int32_t dg;                       /* its step per sample during this group */
    int32_t to;                       /* where the ramp ends, Q16 */
    int32_t t_prev;                   /* the gain the samples in the delay need, Q16 */
} dub_lim = {.g = 65536, .t_prev = 65536, .to = 65536};

static void dub_lim_reset(void)
{
    uint32_t k;
    for (k = 0; k < DUB_LIM_LA; k++)
        dub_lim.dl[k] = dub_lim.dr[k] = 0;
    dub_lim.k = 0;
    dub_lim.env = 0;
    dub_lim.g = dub_lim.t_prev = dub_lim.to = 65536;
    dub_lim.dg = 0;
}

/* the gain (Q16) a group of peak pk needs, from the envelope */
static inline int32_t dub_lim_target(int32_t pk, int32_t ceil)
{
    int32_t e = dub_lim.env;
    e = pk > e ? pk : e - ((e - ceil) >> DUB_RELEASE_SHIFT) - (e > ceil ? 1 : 0);
    if (e < ceil)
        e = ceil;
    dub_lim.env = e;
    return e <= ceil ? 65536 : (int32_t)(((uint32_t)ceil << 16) / (uint32_t)e);
}

static __attribute__((noinline)) void dub_lim_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, j;
    int32_t ceil = dub_ctl.out_q15, reduced = 0;
    if (ceil < 256)
        ceil = 256;
    for (i = 0; i < n; i++) {
        int32_t xl = l[i], xr = r[i], yl, yr, g;
        if (!dub_lim.k) {                               /* a new group: its peak, from the samples ahead in l / r */
            int32_t pk = 0, t, to;
            for (j = i; j < i + DUB_LIM_LA && j < n; j++) {
                int32_t a = l[j] < 0 ? -l[j] : l[j], b = r[j] < 0 ? -r[j] : r[j];
                if (a > pk)
                    pk = a;
                if (b > pk)
                    pk = b;
            }
            t = dub_lim_target(pk, ceil);
            to = t < dub_lim.t_prev ? t : dub_lim.t_prev;   /* the samples now leaving need t_prev, the next t */
            dub_lim.dg = (to - dub_lim.g) >> DUB_LIM_SHIFT;   /* (rounded down: a hair steeper, then snapped) */
            dub_lim.to = to;
            dub_lim.t_prev = t;
        }
        /* out: the sample from 8 ago, at the ramped gain; in: this one */
        yl = dub_lim.dl[dub_lim.k];
        yr = dub_lim.dr[dub_lim.k];
        dub_lim.dl[dub_lim.k] = xl;
        dub_lim.dr[dub_lim.k] = xr;
        dub_lim.g += dub_lim.dg;
        if (++dub_lim.k == DUB_LIM_LA) {
            dub_lim.k = 0;
            dub_lim.g = dub_lim.to;                     /* exact at the group's end (the step was rounded) */
        }
        g = dub_lim.g;
        if (g < 65536) {
            yl = (int32_t)(((int64_t)yl * g) >> 16);
            yr = (int32_t)(((int64_t)yr * g) >> 16);
            reduced = 1;
        }
        l[i] = yl > ceil ? ceil : yl < -ceil ? -ceil : yl;   /* (never needed: the gain already got there) */
        r[i] = yr > ceil ? ceil : yr < -ceil ? -ceil : yr;
    }
    dub_tel.lim_gr_q15 = (uint16_t)(dub_lim.g >= 65536 ? 32767 : dub_lim.g >> 1);
    if (reduced)
        dub_tel.lim_hits++;
}

#endif /* ENABLE_DUB_PREAMP */
