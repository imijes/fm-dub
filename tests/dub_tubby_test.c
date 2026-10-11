/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's stepped high-pass (firmware/src/dub_preamp/dub_tubby_filter.c):
 *   FLAT       step 0, or tubby_on off: the block bit for bit, and the filter's state never touched (no filter
 *              arithmetic runs)
 *   response   every step against the same design in double precision within 0.05 dB (down to -60 dB); the
 *              slope 18 dB / octave; a resonant peak +3.5..+6.5 dB near the cutoff; the passband flat
 *   click      a step change puts one impulse into the filter's input, in that block only: silence in, a ring
 *              out that dies to 0; no change, no click
 *   fades      into (256 samples) and out of (32) FLAT on a bass note: no step larger than the fade-out's own
 *              besides the click
 *   limits     a square far past the clamp: bounded, no overflow (UBSan); silence after: 0 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define ENABLE_DUB_PREAMP
#define main hostsim_main                               /* SLOOP on the host (tables, dsp, the clock): as regress.c */
#include "hostsim.c"
#undef main

static const double HZ[11] = {0, 70, 100, 150, 250, 500, 1000, 2000, 3000, 5000, 7500};
static const double QS[11] = {0, 2.1, 2.16, 2.22, 2.29, 2.36, 2.43, 2.49, 2.57, 2.64, 2.71};
static int fails;
static void check(const char *what, int ok)
{
    printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* the double reference: |H| of the resonant 2-pole HP x the 1-pole HP, from their bilinear designs */
static double ref_db(int step, double f)
{
    double w = 2 * M_PI * HZ[step] / 44100.0, cw = cos(w), al = sin(w) / (2 * QS[step]), a0 = 1 + al;
    double b0 = (1 + cw) / 2 / a0, b1 = -(1 + cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - al) / a0;
    double k = tan(w / 2), g = 1 / (1 + k), p = (1 - k) / (1 + k), o = 2 * M_PI * f / 44100.0;
    double nr = b0 + b1 * cos(o) + b2 * cos(2 * o), ni = -b1 * sin(o) - b2 * sin(2 * o);
    double dr = 1 + a1 * cos(o) + a2 * cos(2 * o), di = -a1 * sin(o) - a2 * sin(2 * o);
    double h2 = (nr * nr + ni * ni) / (dr * dr + di * di);
    double h1n = g * g * ((1 - cos(o)) * (1 - cos(o)) + sin(o) * sin(o));
    double h1d = (1 - p * cos(o)) * (1 - p * cos(o)) + p * p * sin(o) * sin(o);
    return 10 * log10(h2 * h1n / h1d);
}

/* the firmware's gain (dB) at step for a sine of f, steady state */
static double fw_db(int step, double f)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i, t = 0, nb = (uint32_t)(44100.0 * (f < 100 ? 4.0 : 1.5) / CTL);
    double eo = 0, ei = 0;
    memset(&dub_tubby, 0, sizeof dub_tubby);
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = (uint8_t)step;
    for (bl = 0; bl < nb; bl++) {
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (int32_t)lrint(16000 * sin(2 * M_PI * f * t / 44100.0));
        if (bl > nb / 2)
            for (i = 0; i < CTL; i++)
                ei += (double)l[i] * l[i];
        dub_tubby_block(l, r, CTL);
        if (bl > nb / 2)
            for (i = 0; i < CTL; i++)
                eo += (double)l[i] * l[i];
    }
    return 10 * log10((eo + 1e-9) / ei);
}

static void flat(void)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i, same = 1, k;
    const uint8_t *z = (const uint8_t *)dub_tubby.ch;
    int untouched = 1;
    memset(&dub_tubby, 0, sizeof dub_tubby);
    dub_ctl_defaults();
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = 0;
    for (bl = 0; bl < 100u; bl++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)(i * 997u) - 15000;
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            same &= l[i] == (int32_t)(i * 997u) - 15000 && r[i] == l[i];
    }
    for (k = 0; k < sizeof dub_tubby.ch; k++)
        untouched &= z[k] == 0;
    check("step 0 FLAT: bit for bit, the filter never runs (state untouched)", same && untouched);
    dub_ctl.tubby_on = 0;
    dub_ctl.tubby_step = 6;
    same = 1;
    for (bl = 0; bl < 100u; bl++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)(i * 997u) - 15000;
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            same &= l[i] == (int32_t)(i * 997u) - 15000;
    }
    check("tubby_on off (step 6 kept): bit for bit", same && dub_tel.tubby_step == 0);
}

static void response(void)
{
    int s;
    double worst = 0;
    for (s = 1; s <= 10; s++) {
        static const double MUL[] = {0.125, 0.25, 0.5, 0.8, 1.0, 1.2, 1.5, 2.0, 4.0, 8.0};
        double peak = -99, fc = HZ[s];
        uint32_t k;
        printf("  step %2d %5.0f Hz:", s, fc);
        for (k = 0; k < sizeof MUL / sizeof MUL[0]; k++) {
            double f = fc * MUL[k], a, b;
            if (f > 20000 || f < 10)
                continue;
            a = fw_db(s, f);
            b = ref_db(s, f);
            if (b > -60 && fabs(a - b) > worst)
                worst = fabs(a - b);
            if (MUL[k] >= 0.8 && MUL[k] <= 2.0 && a > peak)
                peak = a;
            printf(" %.0f:%+.1f", f, a);
        }
        printf("  (peak %+.1f)\n", peak);
        {
            char t[80];
            snprintf(t, sizeof t, "step %d: a resonant peak near %.0f Hz (+3.5..+6.5 dB)", s, fc);
            check(t, peak > 3.5 && peak < 6.5);
        }
        if (fc * 8 <= 16000) {
            char t[80];
            double pb = fw_db(s, fc * 8);
            snprintf(t, sizeof t, "step %d: passband at 8 x cutoff flat (%+.2f dB)", s, pb);
            check(t, fabs(pb) < 0.6);
        }
        if (fc / 16 >= 12) {
            char t[80];
            double sl = fw_db(s, fc / 8) - fw_db(s, fc / 16);
            snprintf(t, sizeof t, "step %d: slope %.1f dB / octave (18)", s, sl);
            check(t, fabs(sl - 18) < 1.0);
        }
    }
    printf("  worst error against double %.4f dB\n", worst);
    check("every step = the double-precision design within 0.05 dB", worst < 0.05);
}

static void t_click(void)
{
    int32_t l[CTL], r[CTL];
    uint32_t bl, i;
    int32_t loud = 0, tail = 0, quiet = 0;
    memset(&dub_tubby, 0, sizeof dub_tubby);
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = 3;
    for (bl = 0; bl < 3000u; bl++) {                   /* settle at step 3 (its own click dies) */
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        dub_tubby_block(l, r, CTL);
    }
    for (bl = 0; bl < 1000u; bl++) {                   /* no change: silence */
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            if (abs(l[i]) > quiet)
                quiet = abs(l[i]);
    }
    check("no change: no click (silence stays 0)", quiet == 0);
    dub_ctl.tubby_step = 9;                             /* turn: 150 Hz -> 5 kHz */
    for (bl = 0; bl < 6000u; bl++) {
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++) {
            if (bl < 2u && abs(l[i]) > loud)
                loud = abs(l[i]);
            if (bl == 5999u && abs(l[i]) > tail)
                tail = abs(l[i]);
        }
    }
    printf("  click: peak %d, after 4 s %d\n", loud, tail);
    check("a step change: one click that rings at the new step", loud > 500 && loud < 4000);
    check("the click dies away to 0", tail == 0);
}

static void fades(void)
{
    int32_t l[CTL], r[CTL], prev = 0, jump = 0;
    uint32_t bl, i, t = 0;
    memset(&dub_tubby, 0, sizeof dub_tubby);
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = 0;
    for (bl = 0; bl < 900u; bl++) {                    /* a 100 Hz bass: in at 100 (500 Hz step), out at 500 */
        if (bl == 100u)
            dub_ctl.tubby_step = 5;
        if (bl == 500u)
            dub_ctl.tubby_step = 0;
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (int32_t)lrint(16000 * sin(2 * M_PI * 100 * t / 44100.0));
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t d = abs(l[i] - prev);
            if (bl > 2u && !((bl == 100u || bl == 500u) && i <= 1u) && d > jump)   /* (not the click: its sample
                                                                                       * and the one after) */
                jump = d;
            prev = l[i];
        }
    }
    printf("  FLAT in / out on a 100 Hz bass: largest step %d (the fade's own: %d)\n", jump, 16000 / 32);
    check("FLAT in / out: no pop besides the click", jump < 16000 / 32 + 2 * 228);
    check("back in FLAT: the filter stops", dub_tubby.w == 0 && dub_tel.tubby_step == 0);
}

static void limits(void)
{
    int32_t l[CTL], r[CTL], peak = 0, tail = 0;
    uint32_t bl, i, t = 0;
    memset(&dub_tubby, 0, sizeof dub_tubby);
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = 1;
    for (bl = 0; bl < 3000u; bl++) {
        for (i = 0; i < CTL; i++, t++)
            l[i] = r[i] = (t / 300u) & 1u ? (1 << 30) : -(1 << 30);
        dub_tubby_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            if (bl >= 10u && abs(l[i]) > peak)         /* (the first 8 blocks: the fade in, dry unclamped) */
                peak = abs(l[i]);
    }
    for (bl = 0; bl < 20000u; bl++) {
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        dub_tubby_block(l, r, CTL);
    }
    for (i = 0; i < CTL; i++)
        if (abs(l[i]) > tail)
            tail = abs(l[i]);
    printf("  clamp: peak %d (clamp %d), after silence %d\n", peak, DUB_XO_CLAMP, tail);
    check("far past the clamp: bounded (< 4 x the clamp), no overflow", peak < 4 * DUB_XO_CLAMP);
    check("silence after: 0", tail == 0);
}

int main(void)
{
    song.g[G_BPM] = 120;
    flat();
    response();
    t_click();
    fades();
    limits();
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
