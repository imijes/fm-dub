/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's crossover (firmware/src/dub_preamp/dub_crossover.c):
 *   response   each band's gain for sines 20 Hz .. 20 kHz against the same filters in double precision
 *              (LR4 = two Butterworth biquads; the high-pass as two Butterworth HIGH-passes in cascade, not
 *              AP - LP: the identity the firmware relies on is checked here), within 0.02 dB above -40 dB
 *   crossover  at 80 / 300 / 3500 Hz the two neighbours are -6 dB each (LR4)
 *   sum        no kill: the four bands add up to a flat magnitude (allpass) within 0.01 dB
 *   kills      each kill takes its band down by > 60 dB in its range, within one block (32 samples), and
 *              brings it back; all four: silence
 *   meters     a sine in each band lights that band's meter at its level, the others stay low; the release
 *   limits     a full-scale square at 8 x the bus's full scale (the clamp) and beyond: no overflow (UBSan), the
 *              output bounded; silence after: the output settles to 0 (no limit cycle)
 *   bypass     the intercept: BYPASS gives the bus bit for bit; in and out crossfade without a jump
 *   mono low   below 300 Hz the side (L = -R) is gone and a bass on one side comes out on both, at half level;
 *              above, a sound on the left stays on the left (the right gets < -60 dB of it) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define ENABLE_DUB_PREAMP
#define main hostsim_main                               /* SLOOP on the host (tables, dsp, the clock): as regress.c */
#include "hostsim.c"
#undef main

static int fails;
static void check(const char *what, int ok)
{
    printf("%-70s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---- double reference: the same designs, the high-pass as Butterworth high-passes */
typedef struct { double b0, b1, b2, a1, a2, s1, s2; } rbq;
static void rdesign(rbq *f, double fc, int hp)
{
    double w = 2 * M_PI * fc / 44100.0, cw = cos(w), al = sin(w) / (2 * M_SQRT1_2), a0 = 1 + al;
    memset(f, 0, sizeof *f);
    f->a1 = -2 * cw / a0;
    f->a2 = (1 - al) / a0;
    if (hp == 2) {                                      /* allpass */
        f->b0 = f->a2, f->b1 = f->a1, f->b2 = 1;
    } else if (hp) {
        f->b0 = (1 + cw) / 2 / a0, f->b1 = -(1 + cw) / a0, f->b2 = f->b0;
    } else {
        f->b0 = (1 - cw) / 2 / a0, f->b1 = (1 - cw) / a0, f->b2 = f->b0;
    }
}
static double rrun(rbq *f, double x)
{
    double y = f->b0 * x + f->s1;
    f->s1 = f->b1 * x - f->a1 * y + f->s2;
    f->s2 = f->b2 * x - f->a2 * y;
    return y;
}
typedef struct { rbq lp300[2], hp300[2], ph3k5, lp80[2], hp80[2], ph80, lp3k5[2], hp3k5[2]; } rxo;
static void rinit(rxo *r)
{
    int k;
    for (k = 0; k < 2; k++) {
        rdesign(&r->lp300[k], 300, 0), rdesign(&r->hp300[k], 300, 1);
        rdesign(&r->lp80[k], 80, 0), rdesign(&r->hp80[k], 80, 1);
        rdesign(&r->lp3k5[k], 3500, 0), rdesign(&r->hp3k5[k], 3500, 1);
    }
    rdesign(&r->ph3k5, 3500, 2), rdesign(&r->ph80, 80, 2);
}
static void rsplit(rxo *r, double x, double *b)
{
    double lo = rrun(&r->lp300[1], rrun(&r->lp300[0], x)), hi = rrun(&r->hp300[1], rrun(&r->hp300[0], x));
    lo = rrun(&r->ph3k5, lo);
    hi = rrun(&r->ph80, hi);
    b[0] = rrun(&r->lp80[1], rrun(&r->lp80[0], lo));
    b[1] = rrun(&r->hp80[1], rrun(&r->hp80[0], lo));
    b[2] = rrun(&r->lp3k5[1], rrun(&r->lp3k5[0], hi));
    b[3] = rrun(&r->hp3k5[1], rrun(&r->hp3k5[0], hi));
}

/* the firmware's bands, one channel, for a sine: the steady-state gain of each (dB), and of their sum */
static void measure(double f, double amp, double *fw_db, double *ref_db, double *sum_db)
{
    dub_xo_f_t ch;
    rxo r;
    uint32_t n = (uint32_t)(44100.0 * (f < 100 ? 4.0 : 1.0)), i, k, settle = n / 2;
    double e[4] = {0}, er[4] = {0}, es = 0, ex = 0;
    memset(&ch, 0, sizeof ch);
    rinit(&r);
    for (i = 0; i < n; i++) {
        double x = amp * sin(2 * M_PI * f * i / 44100.0), rb[4];
        int32_t xi = (int32_t)lrint(x), b[4], b2[4];
        dub_xo_split2(&ch, xi * (1 << DUB_XO_SHIFT), xi * (1 << DUB_XO_SHIFT), b, b2);   /* (centred: L = R) */
        rsplit(&r, (double)xi, rb);
        if (i >= settle) {
            for (k = 0; k < 4; k++) {
                double v = b[k] / (double)(1 << DUB_XO_SHIFT);
                e[k] += v * v;
                er[k] += rb[k] * rb[k];
            }
            {
                double s = (b[0] + b[1] + b[2] + b[3]) / (double)(1 << DUB_XO_SHIFT);
                es += s * s;
            }
            ex += (double)xi * xi;
        }
    }
    for (k = 0; k < 4; k++) {
        fw_db[k] = 10 * log10((e[k] + 1e-30) / ex);
        ref_db[k] = 10 * log10((er[k] + 1e-30) / ex);
    }
    *sum_db = 10 * log10(es / ex);
}

static void response(void)
{
    static const double F[] = {20, 30, 40, 60, 80, 100, 150, 200, 300, 450, 700, 1000, 2000, 3500, 5000,
                               8000, 12000, 16000, 20000};
    double worst = 0, sumdev = 0, fw[4], rf[4], s;
    uint32_t i, k;
    for (i = 0; i < sizeof F / sizeof F[0]; i++) {
        measure(F[i], 16000, fw, rf, &s);
        printf("  %6.0f Hz  SUB %7.2f  BASS %7.2f  MID %7.2f  TREBLE %7.2f  sum %+.3f dB\n", F[i], fw[0], fw[1],
               fw[2], fw[3], s);
        for (k = 0; k < 4; k++)
            if (rf[k] > -40 && fabs(fw[k] - rf[k]) > worst)
                worst = fabs(fw[k] - rf[k]);
        if (fabs(s) > sumdev)
            sumdev = fabs(s);
    }
    printf("  worst band error against double %.4f dB, sum deviation %.4f dB\n", worst, sumdev);
    check("each band = the LR4 reference (Butterworth HP cascade) within 0.02 dB", worst < 0.02);
    check("no kill: the bands sum flat within 0.01 dB, 20 Hz .. 20 kHz", sumdev < 0.01);
    measure(80, 16000, fw, rf, &s);
    check("80 Hz: SUB and BASS -6 dB each", fabs(fw[0] + 6.02) < 0.1 && fabs(fw[1] + 6.02) < 0.1);
    measure(300, 16000, fw, rf, &s);
    check("300 Hz: BASS and MID -6 dB each", fabs(fw[1] + 6.02) < 0.15 && fabs(fw[2] + 6.02) < 0.15);
    measure(3500, 16000, fw, rf, &s);
    check("3.5 kHz: MID and TREBLE -6 dB each", fabs(fw[2] + 6.02) < 0.1 && fabs(fw[3] + 6.02) < 0.1);
}

/* dub_xo_block on a stereo sine for `blocks` blocks; the output's RMS (dB re the input) over the last half */
static double block_gain(double f, uint32_t blocks, uint32_t kill)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i, t = 0;
    double eo = 0, ei = 0;
    dub_ctl.kill = (uint8_t)kill;
    for (bl = 0; bl < blocks; bl++) {
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (int32_t)lrint(16000 * sin(2 * M_PI * f * t / 44100.0));
        if (bl >= blocks / 2)
            for (i = 0; i < CTL; i++)
                ei += (double)l[i] * l[i];
        dub_xo_block(l, r, CTL);
        if (bl >= blocks / 2)
            for (i = 0; i < CTL; i++)
                eo += (double)l[i] * l[i];
    }
    return 10 * log10((eo + 1e-9) / ei);
}

/* a stereo sine through dub_xo_block, with a twin of the filters (dub_xo_split on the same input) giving the
 * exact bands: after the kill ramp, the output must be the sum of the bands not killed, to the rounding */
static int32_t kill_err(double f, uint32_t kill, uint32_t blocks, uint32_t from_block)
{
    int32_t l[CTL], r[CTL], worst = 0;
    dub_xo_f_t twin;
    uint32_t bl, i, t = 0, k;
    dub_xo_reset();
    for (k = 0; k < 4; k++)
        dub_xo.g[k] = 32767;
    memset(&twin, 0, sizeof twin);
    dub_ctl.kill = 0;
    for (bl = 0; bl < blocks; bl++) {
        if (bl == blocks / 2)
            dub_ctl.kill = (uint8_t)kill;
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (int32_t)lrint(16000 * sin(2 * M_PI * f * t / 44100.0));
        {
            int32_t want[CTL];
            for (i = 0; i < CTL; i++) {
                int32_t b[4], b2[4], sum = 0;
                dub_xo_split2(&twin, l[i] * (1 << DUB_XO_SHIFT), r[i] * (1 << DUB_XO_SHIFT), b, b2);
                for (k = 0; k < 4; k++)
                    if (!((kill >> k) & 1u) || bl < blocks / 2)
                        sum += b[k];
                want[i] = (sum + (1 << (DUB_XO_SHIFT - 1))) >> DUB_XO_SHIFT;
            }
            dub_xo_block(l, r, CTL);
            if (bl >= blocks / 2 + from_block)
                for (i = 0; i < CTL; i++)
                    if (abs(l[i] - want[i]) > worst)
                        worst = abs(l[i] - want[i]);
        }
    }
    dub_ctl.kill = 0;
    return worst;
}

static void kills(void)
{
    static const double MID_F[4] = {40, 160, 1000, 9000};
    uint32_t b;
    dub_ctl_defaults();
    for (b = 0; b < 4; b++) {
        char t[96];
        int32_t e = kill_err(MID_F[b], 1u << b, 400, 1);
        snprintf(t, sizeof t, "kill band %u (%.0f Hz): from the next block = the other three bands (err %d)", b,
                 MID_F[b], e);
        check(t, e <= 1);
    }
    check("kill SUB + MID together: = BASS + TREBLE", kill_err(150, DUB_KILL_SUB | DUB_KILL_MID, 400, 1) <= 1);
    check("all four killed: silence", block_gain(1000, 300, DUB_KILL_ALL) < -90 && block_gain(50, 300, DUB_KILL_ALL) < -90);
    {   /* released: back to the full sum (the open path) within a block */
        int32_t e;
        dub_xo_reset();
        block_gain(1000, 100, DUB_KILL_MID);
        e = kill_err(1000, 0, 200, 1);
        check("a kill released: the full sum again", e <= 1);
    }
    dub_ctl.kill = 0;
}

static void meters(void)
{
    static const double MID_F[4] = {500, 160, 1000, 9000};
    static const double LOW_F = 40;
    uint32_t b, k;
    for (b = 0; b < 4; b++) {
        double f = b == 0 ? LOW_F : MID_F[b], fw[4], rf[4], s;
        int ok = 1;
        measure(f, 16000, fw, rf, &s);
        dub_xo_reset();
        for (k = 0; k < 4; k++)
            dub_xo.env[k] = 0;
        block_gain(f, 1200, 0);
        for (k = 0; k < 4; k++) {
            double want = 16000 * pow(10, fw[k] / 20);   /* the band's own level at f */
            double tol = want * (b == 0 ? 0.08 : 0.03) + 20;   /* (40 Hz: the release dips between the peaks) */
            ok &= fabs(dub_tel.band_pk[k] - want) <= tol || (k == b ? 0 : dub_tel.band_pk[k] <= want * 1.05 + 20);
        }
        printf("  %5.0f Hz: meters %5u %5u %5u %5u\n", f, dub_tel.band_pk[0], dub_tel.band_pk[1],
               dub_tel.band_pk[2], dub_tel.band_pk[3]);
        {
            char t[80];
            snprintf(t, sizeof t, "meters: a %.0f Hz sine reads each band at its level", f);
            check(t, ok);
        }
    }
    {   /* release: silence for 0.43 s takes the meter to ~-20 dB */
        int32_t l[CTL] = {0}, r[CTL] = {0};
        uint16_t p0 = dub_tel.band_pk[3];
        for (k = 0; k < 588u; k++) {
            memset(l, 0, sizeof l), memset(r, 0, sizeof r);
            dub_xo_block(l, r, CTL);
        }
        check("meter release: -20 dB in ~0.43 s", dub_tel.band_pk[3] > p0 / 14 && dub_tel.band_pk[3] < p0 / 7);
    }
}

static void limits(void)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i, t = 0;
    int32_t peak = 0, tail = 0;
    dub_xo_reset();
    dub_ctl.kill = 0;
    for (bl = 0; bl < 2000u; bl++) {                   /* a 50 Hz square at +-2^30 (far past the clamp) */
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (t / 441u) & 1u ? (1 << 30) : -(1 << 30);
        dub_xo_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            if (abs(l[i]) > peak)
                peak = abs(l[i]);
    }
    for (bl = 0; bl < 20000u; bl++) {                  /* then 14 s of silence */
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        dub_xo_block(l, r, CTL);
    }
    for (i = 0; i < CTL; i++)
        if (abs(l[i]) > tail)
            tail = abs(l[i]);
    printf("  clamp: output peak %d (8 x full scale = %d), after silence %d\n", peak, DUB_XO_CLAMP, tail);
    check("beyond the clamp: bounded (< 2 x the clamp), no overflow", peak < 2 * DUB_XO_CLAMP);
    check("silence after: the output settles to 0", tail == 0);
}

static const int32_t zs[CTL];                           /* (no delay / reverb sends) */
static void bypass(void)
{
    int32_t l[CTL], r[CTL], prev = 0, jump = 0;
    uint32_t bl, i, t = 0, same = 1;
    dub_ctl_defaults();
    dub_ctl.usb_in_q15 = 0;
    dub_ctl.bypass = 1;
    dub_chain.w = 0;
    for (bl = 0; bl < 50u; bl++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)(i * 777u) - 12000;
        dub_mix_intercept(l, r, zs, zs, CTL);
        for (i = 0; i < CTL && bl; i++)                 /* (8 samples later: the limiter's look-ahead, always on) */
            same &= l[i] == (int32_t)((i + CTL - 8u) % CTL * 777u) - 12000;
    }
    check("BYPASS: the bus bit for bit, 8 samples later (the colour does not run)", same && dub_chain.cold);
    for (bl = 0; bl < 600u; bl++) {                    /* a 100 Hz sine; BYPASS off at 100, on at 400 */
        if (bl == 100u)
            dub_ctl.bypass = 0;
        if (bl == 400u)
            dub_ctl.bypass = 1;
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (int32_t)lrint(16000 * sin(2 * M_PI * 100 * t / 44100.0));
        dub_mix_intercept(l, r, zs, zs, CTL);
        for (i = 0; i < CTL; i++) {
            if (bl > 2u && abs(l[i] - prev) > jump)
                jump = abs(l[i] - prev);
            prev = l[i];
        }
    }
    printf("  bypass in / out on a 100 Hz sine: largest step between samples %d (the sine's own: %d)\n", jump,
           (int)lrint(16000 * 2 * M_PI * 100 / 44100.0));
    check("BYPASS in and out: no jump (steps within 2 x the sine's own)", jump < 2 * 16000 * 2 * M_PI * 100 / 44100.0);
}

/* RMS (dB re 16000 peak sine) of the left and right outputs for a sine of f on the left (and gr x it on the right) */
static void stereo_gain(double f, double gr, double *dl, double *dr)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i, t = 0;
    double el = 0, er = 0, ei = 0;
    dub_xo_reset();
    dub_ctl.kill = 0;
    for (bl = 0; bl < 2400u; bl++) {
        for (i = 0; i < CTL; i++, t++) {
            double x = 16000 * sin(2 * M_PI * f * t / 44100.0);
            l[i] = (int32_t)lrint(x);
            r[i] = (int32_t)lrint(gr * x);
            if (bl >= 1200u)
                ei += x * x;
        }
        dub_xo_block(l, r, CTL);
        if (bl >= 1200u)
            for (i = 0; i < CTL; i++)
                el += (double)l[i] * l[i], er += (double)r[i] * r[i];
    }
    *dl = 10 * log10((el + 1e-9) / ei);
    *dr = 10 * log10((er + 1e-9) / ei);
}

static void mono_low(void)
{
    double dl, dr;
    stereo_gain(60, -1.0, &dl, &dr);                    /* side only, 60 Hz */
    printf("  side (L = -R) 60 Hz: L %.1f dB, R %.1f dB\n", dl, dr);
    check("mono low end: a side-only 60 Hz is gone (< -30 dB)", dl < -30 && dr < -30);
    stereo_gain(60, 0.0, &dl, &dr);                     /* left only, 60 Hz */
    printf("  left only 60 Hz: L %.2f dB, R %.2f dB\n", dl, dr);
    check("mono low end: a 60 Hz on the left plays on both sides at -6 dB", fabs(dl + 6.02) < 0.3 && fabs(dr + 6.02) < 0.3);
    stereo_gain(2000, 0.0, &dl, &dr);                   /* left only, 2 kHz */
    printf("  left only 2 kHz: L %.2f dB, R %.1f dB\n", dl, dr);
    check("stereo highs: a 2 kHz on the left stays left (R < -60 dB)", fabs(dl) < 0.3 && dr < -60);
}

int main(void)
{
    song.g[G_BPM] = 120;
    response();
    kills();
    meters();
    limits();
    bypass();
    mono_low();
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
