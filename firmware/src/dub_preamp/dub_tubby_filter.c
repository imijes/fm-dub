/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 3.2: the "big knob", King Tubby's stepped high-pass (after the Altec 9069B).
 *
 *   step  0 FLAT   1 70 Hz   2 100   3 150   4 250   5 500   6 1 k   7 2 k   8 3 k   9 5 k   10 7.5 kHz
 *
 * 18 dB / octave: a resonant 2-pole high-pass (Q 2.1 at 70 Hz up to 2.71 at 7.5 kHz) then a 1-pole high-pass at
 * the same cutoff, per channel: a ringing peak of +4 dB (70 Hz) rising to +6 dB (7.5 kHz) just above the cutoff. DUB_TUBBY_COEF (tools/gen_dub_tubby.py, 44.1
 * kHz, Q30) has a row per step; row 0 (FLAT) is the identity and is never run: at step 0, or with dub_ctl.tubby_on
 * off, the block goes straight through with no filter arithmetic at all (an explicit branch, no neutral
 * coefficients).
 *
 * The switch: a change of step puts one impulse (DUB_TUBBY_CLICK, ~-27 dBFS, one sample, at the start of that
 * block) into the filter's input: the rotary contacts' click, ringing at the new step's resonance. Step to step,
 * the coefficients change at the block boundary and the state carries on (as the real switch: no reset). Into
 * and out of FLAT the filter fades against the dry signal, so taking the bass away is the click, not a pop the
 * size of the bass: in over 256 samples (its state starts from 0, and the onset rings at the resonance for ~1.5
 * ms: the fade covers it), out over 32.
 *
 * Numbers as the crossover's (dub_crossover.c): the bus Q15 << 10 in 32 bits, clamped at 8 x full scale; the
 * biquad DF2T with 64-bit state and its rounding remainder fed back (dub_bq); the 1-pole keeps its remainder too
 * (a 70 Hz pole would otherwise lift its rounding by 40 dB at DC).
 *
 * Audio ISR only (dub_mix_intercept, before the crossover; after the saturation of Phase 3.3). */
#ifdef ENABLE_DUB_PREAMP

typedef struct {
    dub_bq_coef_t bq;                 /* the resonant 2-pole high-pass */
    int32_t g, p;                     /* the 1-pole: y = g (x - x1) + p y1 */
} dub_tubby_coef_t;

/* tools/gen_dub_tubby.py 44100: b0, b1, b2, a1, a2, g, p (Q30) */
static const dub_tubby_coef_t DUB_TUBBY_COEF[DUB_TUBBY_NSTEP] = {
    {{1073741824, 0, 0, 0, 0}, 1073741824, 0},   /* 0 FLAT: never run */
    {{1071171564, -2142343129, 1071171564, -2142289855, 1068654579}, 1068413968, 1063086112},   /* 1: 70 Hz, Q 2.1 */
    {{1070158017, -2140316033, 1070158017, -2140207412, 1066682830}, 1066146687, 1058551551},   /* 2: 100 Hz, Q 2.16 */
    {{1068476639, -2136953278, 1068476639, -2136709253, 1063455478}, 1062389033, 1051036242},   /* 3: 150 Hz, Q 2.22 */
    {{1065119539, -2130239078, 1065119539, -2129563271, 1057173061}, 1054951694, 1036161564},   /* 4: 250 Hz, Q 2.29 */
    {{1056448863, -2112897727, 1056448863, -2110214802, 1041838827}, 1036796578, 999851332},   /* 5: 500 Hz, Q 2.36 */
    {{1037975499, -2075950998, 1037975499, -2065380129, 1012780044}, 1002224412, 930707000},   /* 6: 1000 Hz, Q 2.43 */
    {{995877328, -1991754657, 995877328, -1950769777, 958997713}, 939038772, 804335719},   /* 7: 2000 Hz, Q 2.49 */
    {{948914177, -1897828354, 948914177, -1808438418, 913476467}, 882265477, 690789129},   /* 8: 3000 Hz, Q 2.57 */
    {{839280980, -1678561960, 839280980, -1446202333, 837179762}, 782577088, 491412352},   /* 9: 5000 Hz, Q 2.64 */
    {{684597478, -1369194956, 684597478, -889848939, 774799149}, 674593639, 275445455},   /* 10: 7500 Hz, Q 2.71 */
};
_Static_assert(DUB_FS == 44100, "DUB_TUBBY_COEF is for 44.1 kHz: run tools/gen_dub_tubby.py for another rate");
_Static_assert(DUB_TUBBY_NSTEP == 11u, "11 steps: FLAT and ten cutoffs");

#define DUB_TUBBY_CLICK 1500          /* Q15: the contacts' impulse */
#define DUB_TUBBY_FADE_IN 128         /* Q15 per sample: 256 samples (5.8 ms) out of FLAT: the filter starts from 0
                                       * and a held bass hits its resonance as an onset; the fade covers that ring */
#define DUB_TUBBY_FADE_OUT 1024       /* 32 samples back to FLAT (the state is warm: nothing to cover) */

typedef struct {
    dub_bq_t bq;
    int32_t x1, y1;                   /* the 1-pole's last input and output */
    int64_t rem;                      /* its rounding remainder, carried */
} dub_tubby_ch_t;

static struct {
    dub_tubby_ch_t ch[2];
    uint8_t cur;                      /* the step playing (0: FLAT) */
    uint8_t coef;                     /* the row in use: cur, or the last step while fading out to FLAT */
    int32_t w;                        /* the filter's share against dry, Q15 (32767 in, 0 FLAT) */
} dub_tubby;

static inline int32_t dub_tubby_ch(const dub_tubby_coef_t *c, dub_tubby_ch_t *z, int32_t x)
{
    int32_t v = dub_bq(&c->bq, &z->bq, x), y;
    int64_t acc = (int64_t)c->g * (v - z->x1) + (int64_t)c->p * z->y1 + z->rem;
    y = (int32_t)((acc + (1LL << 29)) >> 30);
    z->rem = acc - (int64_t)y * (1LL << 30);
    z->x1 = v;
    z->y1 = y;
    return y;
}

static void dub_tubby_reset(void)
{
    uint32_t k;
    uint8_t *p = (uint8_t *)dub_tubby.ch;
    for (k = 0; k < sizeof dub_tubby.ch; k++)
        p[k] = 0;
}

/* the block l / r (Q15 in 32 bits, n <= CTL) through the stepped high-pass, in place */
static __attribute__((noinline)) void dub_tubby_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, step = dub_ctl.tubby_on ? dub_ctl.tubby_step : 0u;
    const dub_tubby_coef_t *c;
    if (step >= DUB_TUBBY_NSTEP)
        step = DUB_TUBBY_NSTEP - 1u;
    if (step != dub_tubby.cur) {                        /* the knob moved: the contacts click */
        if (!dub_tubby.cur && !dub_tubby.w)
            dub_tubby_reset();                          /* (coming in from FLAT: no stale state) */
        dub_tubby.cur = (uint8_t)step;
        if (step)
            dub_tubby.coef = (uint8_t)step;             /* (to FLAT: the last step's row fades out) */
        if (n) {
            l[0] += DUB_TUBBY_CLICK;
            r[0] += DUB_TUBBY_CLICK;
        }
    }
    dub_tel.tubby_step = dub_tubby.cur;
    if (!dub_tubby.cur && !dub_tubby.w)                 /* FLAT: straight through, no filter at all */
        return;
    c = &DUB_TUBBY_COEF[dub_tubby.coef];
    for (i = 0; i < n; i++) {
        int32_t xl = l[i], xr = r[i], yl, yr, w = dub_tubby.w, to = dub_tubby.cur ? 32767 : 0;
        xl = xl > DUB_XO_CLAMP ? DUB_XO_CLAMP : xl < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xl;
        xr = xr > DUB_XO_CLAMP ? DUB_XO_CLAMP : xr < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xr;
        yl = dub_tubby_ch(c, &dub_tubby.ch[0], xl * (1 << DUB_XO_SHIFT));
        yr = dub_tubby_ch(c, &dub_tubby.ch[1], xr * (1 << DUB_XO_SHIFT));
        yl = (yl + (1 << (DUB_XO_SHIFT - 1))) >> DUB_XO_SHIFT;
        yr = (yr + (1 << (DUB_XO_SHIFT - 1))) >> DUB_XO_SHIFT;
        if (w != to) {                                  /* into / out of FLAT */
            w = w < to ? (to - w > DUB_TUBBY_FADE_IN ? w + DUB_TUBBY_FADE_IN : to)
                       : (w - to > DUB_TUBBY_FADE_OUT ? w - DUB_TUBBY_FADE_OUT : to);
            dub_tubby.w = w;
        }
        if (w == 32767) {
            l[i] = yl;
            r[i] = yr;
        } else {
            l[i] += (int32_t)(((int64_t)(yl - l[i]) * w) >> 15);
            r[i] += (int32_t)(((int64_t)(yr - r[i]) * w) >> 15);
        }
    }
}

#endif /* ENABLE_DUB_PREAMP */
