/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 3.1: the 4-way crossover, the band kills, the band meters.
 *
 *   SUB 20..80 Hz   BASS 80..300 Hz   MID 300 Hz..3.5 kHz   TREBLE 3.5..20 kHz
 *
 * Linkwitz-Riley 4th order (LR4) at 80, 300 and 3500 Hz, as a tree. Each channel is split at 300 Hz; below
 * that, the two low branches are summed to mono ((L + R) / 2: a centred bass keeps its level) and SUB and BASS
 * are split once, on the mono signal, and go to both sides. MID and TREBLE stay stereo:
 *
 *   L -+- LP300 LP300 -- lowL -.
 *      '- AP300 - lowL -- highL -> AP80 (phase) -+- LP3k5 LP3k5 -- MID L;  AP3k5 - MID L -- TREBLE L
 *                              +- (lowL + lowR) / 2 -> AP3500 (phase) -+- LP80 LP80 ----- SUB  (L and R)
 *                              |                                       '- AP80 - SUB ---- BASS (L and R)
 *   R -+- LP300 LP300 -- lowR -'
 *      '- AP300 - lowR -- highR -> AP80 (phase) -+- LP3k5 LP3k5 -- MID R;  AP3k5 - MID R -- TREBLE R
 *
 * LP is the 2nd-order Butterworth low-pass (two in cascade: the LR4 low-pass). LR4 low + LR4 high is the
 * 2nd-order allpass AP with the same poles, so the LR4 high-pass is taken as AP - LP4: the same filter as two
 * Butterworth high-passes in cascade, one biquad fewer, and low + high = AP to the last bit. Each branch also
 * gets the other split's allpass, so for a centred (mono) input SUB + BASS + MID + TREBLE = AP80 AP300 AP3500 x:
 * a flat magnitude (phase only) with no band killed. Below 300 Hz the side (L - R) is gone: the dub sound
 * system's mono low end. 7 biquads per channel and 4 for the mono low end: 18 a stereo sample (22 all stereo).
 *
 * Numbers (no float): coefficients Q30 (DUB_XO_COEF: tools/gen_dub_crossover.py, at FS 44.1 kHz), samples in
 * 32 bits at the bus's Q15 << 10 (full scale 2^25; the bus is clamped at 8 x full scale, so every sum, the
 * high = AP - low ones too, stays inside 2^31). Biquads in direct form II transposed with 64-bit state: each
 * product kept whole, the output rounded back by >> 30 (the coefficients' Q30) once per biquad, its remainder
 * fed back too (dub_bq).
 *
 * Kills: dub_kill_now() (buttons OR MIDI), read once a block. A killed band's gain goes to 0 in 32 samples
 * (0.73 ms, a linear ramp: no click) and comes back the same way; no delay is added. Meters: dub_tel.band_pk,
 * each band's peak after its kill gain, Q15: instant attack, release ~0.43 s to -20 dB (the VU pillars).
 *
 * Audio ISR only (dub_mix_intercept). */
#ifdef ENABLE_DUB_PREAMP

typedef struct { int32_t b0, b1, b2, a1, a2; } dub_bq_coef_t;   /* Q30; a0 = 1 */
typedef struct { int64_t s1, s2; } dub_bq_t;

enum { DUB_XO_LP80, DUB_XO_AP80, DUB_XO_LP300, DUB_XO_AP300, DUB_XO_LP3K5, DUB_XO_AP3K5, DUB_XO_NCOEF };
/* tools/gen_dub_crossover.py 44100: b0, b1, b2, a1, a2 (Q30) */
static const dub_bq_coef_t DUB_XO_COEF[DUB_XO_NCOEF] = {
    {34595, 69190, 34595, -2130176048, 1056572604},   /* LP 80 Hz */
    {1056572604, -2130176048, 1073741824, -2130176048, 1056572604},   /* AP 80 Hz */
    {475960, 951923, 475960, -2082597652, 1010759671},   /* LP 300 Hz */
    {1010759671, -2082597652, 1073741824, -2082597652, 1010759671},   /* AP 300 Hz */
    {48856999, 97714000, 48856999, -1409355405, 531041579},   /* LP 3500 Hz */
    {531041579, -1409355405, 1073741824, -1409355405, 531041579},   /* AP 3500 Hz */
};
_Static_assert(DUB_FS == 44100, "DUB_XO_COEF is for 44.1 kHz: run tools/gen_dub_crossover.py for another rate");

#define DUB_XO_SHIFT 10                       /* bus Q15 -> the crossover's 2^25 full scale */
#define DUB_XO_CLAMP (8 * 32768)              /* the bus, at most 8 x full scale (+18 dB) */
#define DUB_XO_RAMP 1024                      /* kill gain step per sample, Q15: 32 samples end to end */

/* the filters: per channel the 300 Hz split and the high branch, once the mono low branch */
typedef struct {
    dub_bq_t lp300a, lp300b, ap300;           /* the 300 Hz split */
    dub_bq_t ph80;                            /* the high branch's AP80 (phase) */
    dub_bq_t lp3k5a, lp3k5b, ap3k5;           /* the 3.5 kHz split */
} dub_xo_ch_t;
typedef struct {
    dub_bq_t ph3k5;                           /* the low branch's AP3500 (phase) */
    dub_bq_t lp80a, lp80b, ap80;              /* the 80 Hz split */
} dub_xo_lo_t;
typedef struct {
    dub_xo_ch_t ch[2];
    dub_xo_lo_t lo;
} dub_xo_f_t;

static struct {
    dub_xo_f_t f;
    int32_t g[DUB_NBAND];                     /* kill gains, Q15 (32767 = open) */
    uint32_t env[DUB_NBAND];                  /* meters, Q15 */
    uint8_t want_sub;                         /* the sub bloom listens (dub_limiter.c): keep the SUB band in sub[] */
    int32_t sub[CTL];                         /* the mono SUB band of the last block, Q15 (before its kill) */
} dub_xo = {.g = {32767, 32767, 32767, 32767}};

/* y = b0 x + s1;  s1 = b1 x - a1 y + s2;  s2 = b2 x - a2 y   (DF2T, the state at the products' Q30 scale)
 * The output is rounded to 32 bits, but the feedback uses it whole: the remainder e (Q30 of an output step) goes
 * back through a1 and a2 too. Without that the rounding re-enters through 1 / A(z), which at a low cutoff is
 * huge (1 / (1 + a1 + a2): +77 dB at 80 Hz): a noise floor near -70 dB and a small limit cycle after silence.
 * Two more multiplies; the bands' floor goes below -110 dB and the state settles to 0. */
static __attribute__((noinline)) int32_t dub_bq(const dub_bq_coef_t *c, dub_bq_t *z, int32_t x)   /* (out of line:
                                                         * the flash is full; 11 calls a sample) */
{
    int64_t acc = (int64_t)c->b0 * x + z->s1;
    int32_t y = (int32_t)((acc + (1LL << 29)) >> 30);
    int64_t e = acc - (int64_t)y * (1LL << 30);        /* -2^29 .. 2^29: what the rounding left out of y */
    z->s1 = (int64_t)c->b1 * x - (int64_t)c->a1 * y - (((int64_t)c->a1 * e) >> 30) + z->s2;
    z->s2 = (int64_t)c->b2 * x - (int64_t)c->a2 * y - (((int64_t)c->a2 * e) >> 30);
    return y;
}
/* the same without the remainder: for the 3.5 kHz filters, where 1 / (1 + a1 + a2) is only +15 dB (the floor
 * stays below -120 dB) */
static __attribute__((noinline)) int32_t dub_bq_hi(const dub_bq_coef_t *c, dub_bq_t *z, int32_t x)   /* (out of
                                                         * line: 7 calls a sample, ~1.4 kB less flash, which is full) */
{
    int64_t acc = (int64_t)c->b0 * x + z->s1;
    int32_t y = (int32_t)((acc + (1LL << 29)) >> 30);
    z->s1 = (int64_t)c->b1 * x - (int64_t)c->a1 * y + z->s2;
    z->s2 = (int64_t)c->b2 * x - (int64_t)c->a2 * y;
    return y;
}

/* one channel at 300 Hz: its low part out (*lo), its MID and TREBLE into band[2..3] */
static inline void dub_xo_split_ch(dub_xo_ch_t *f, int32_t x, int32_t *lo, int32_t *band)
{
    const dub_bq_coef_t *c = DUB_XO_COEF;
    int32_t l, hi, a;
    l = dub_bq(&c[DUB_XO_LP300], &f->lp300b, dub_bq(&c[DUB_XO_LP300], &f->lp300a, x));
    hi = dub_bq(&c[DUB_XO_AP300], &f->ap300, x) - l;
    hi = dub_bq(&c[DUB_XO_AP80], &f->ph80, hi);
    a = dub_bq_hi(&c[DUB_XO_LP3K5], &f->lp3k5b, dub_bq_hi(&c[DUB_XO_LP3K5], &f->lp3k5a, hi));
    band[DUB_MID] = a;
    band[DUB_TREBLE] = dub_bq_hi(&c[DUB_XO_AP3K5], &f->ap3k5, hi) - a;
    *lo = l;
}

/* one stereo sample into its bands: bl[] left, br[] right; SUB and BASS (mono) the same in both */
static inline void dub_xo_split2(dub_xo_f_t *f, int32_t xl, int32_t xr, int32_t *bl, int32_t *br)
{
    const dub_bq_coef_t *c = DUB_XO_COEF;
    int32_t lol, lor, lo, a;
    dub_xo_split_ch(&f->ch[0], xl, &lol, bl);
    dub_xo_split_ch(&f->ch[1], xr, &lor, br);
    lo = (int32_t)(((int64_t)lol + lor) >> 1);            /* the mono low end */
    lo = dub_bq_hi(&c[DUB_XO_AP3K5], &f->lo.ph3k5, lo);
    a = dub_bq(&c[DUB_XO_LP80], &f->lo.lp80b, dub_bq(&c[DUB_XO_LP80], &f->lo.lp80a, lo));
    bl[DUB_SUB] = br[DUB_SUB] = a;
    bl[DUB_BASS] = br[DUB_BASS] = dub_bq(&c[DUB_XO_AP80], &f->lo.ap80, lo) - a;
}

/* every filter state back to 0 (the preamp coming out of BYPASS: no stale state) */
static void dub_xo_reset(void)
{
    uint32_t k;
    uint8_t *p = (uint8_t *)&dub_xo.f;
    for (k = 0; k < sizeof dub_xo.f; k++)
        p[k] = 0;
}

/* the block l / r (Q15 in 32 bits, n <= CTL) through the crossover, the kills, the meters; in place */
static __attribute__((noinline)) void dub_xo_block(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, b, kill = dub_kill_now();
    int32_t t[DUB_NBAND];
    uint32_t pk[DUB_NBAND] = {0, 0, 0, 0};
    int open = 1;
    for (b = 0; b < DUB_NBAND; b++) {
        t[b] = (kill >> b) & 1u ? 0 : 32767;
        open &= t[b] == 32767 && dub_xo.g[b] == 32767;
    }
    for (i = 0; i < n; i++) {
        int32_t bl[DUB_NBAND], br[DUB_NBAND], xl = l[i], xr = r[i];
        xl = xl > DUB_XO_CLAMP ? DUB_XO_CLAMP : xl < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xl;
        xr = xr > DUB_XO_CLAMP ? DUB_XO_CLAMP : xr < -DUB_XO_CLAMP ? -DUB_XO_CLAMP : xr;
        dub_xo_split2(&dub_xo.f, xl * (1 << DUB_XO_SHIFT), xr * (1 << DUB_XO_SHIFT), bl, br);   /* (a multiply:
                                                                                    * << of a negative is UB) */
        if (dub_xo.want_sub && i < CTL)
            dub_xo.sub[i] = bl[DUB_SUB] >> DUB_XO_SHIFT;   /* (mono: the same in br) */
        if (open) {                           /* nothing killed: the plain sum */
            int32_t sl = bl[0] + bl[1] + bl[2] + bl[3], sr = br[0] + br[1] + br[2] + br[3];
            for (b = 0; b < DUB_NBAND; b++) {
                uint32_t a = (uint32_t)(bl[b] < 0 ? -bl[b] : bl[b]), c = (uint32_t)(br[b] < 0 ? -br[b] : br[b]);
                a = a > c ? a : c;
                if (a > pk[b])
                    pk[b] = a;
            }
            l[i] = (sl + (1 << (DUB_XO_SHIFT - 1))) >> DUB_XO_SHIFT;
            r[i] = (sr + (1 << (DUB_XO_SHIFT - 1))) >> DUB_XO_SHIFT;
        } else {                              /* kills: each band by its ramped gain */
            int64_t sl = 0, sr = 0;
            for (b = 0; b < DUB_NBAND; b++) {
                int32_t g = dub_xo.g[b];
                int64_t vl, vr;
                uint32_t a, c;
                if (g != t[b])
                    g = g < t[b] ? (t[b] - g > DUB_XO_RAMP ? g + DUB_XO_RAMP : t[b])
                                 : (g - t[b] > DUB_XO_RAMP ? g - DUB_XO_RAMP : t[b]);
                dub_xo.g[b] = g;
                vl = (int64_t)bl[b] * g;
                vr = (int64_t)br[b] * g;
                sl += vl;
                sr += vr;
                a = (uint32_t)((vl < 0 ? -vl : vl) >> 15);
                c = (uint32_t)((vr < 0 ? -vr : vr) >> 15);
                a = a > c ? a : c;
                if (a > pk[b])
                    pk[b] = a;
            }
            l[i] = (int32_t)((sl + (1LL << (DUB_XO_SHIFT + 14))) >> (DUB_XO_SHIFT + 15));
            r[i] = (int32_t)((sr + (1LL << (DUB_XO_SHIFT + 14))) >> (DUB_XO_SHIFT + 15));
        }
    }
    for (b = 0; b < DUB_NBAND; b++) {         /* meters: instant attack, release (1 - 2^-8) a block */
        uint32_t p = pk[b] >> DUB_XO_SHIFT, e = dub_xo.env[b];
        e = p > e ? p : e - (e >> 8) - (e ? 1u : 0u);
        dub_xo.env[b] = e;
        dub_tel.band_pk[b] = (uint16_t)(e > 32767u ? 32767u : e);
    }
}

#endif /* ENABLE_DUB_PREAMP */
