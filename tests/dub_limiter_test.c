/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's sub bloom and safety limiter (firmware/src/dub_preamp/dub_limiter.c), on SLOOP's
 * host build (hostsim.c). The transport clock is driven as seq.c events_block does it: before a block is mixed,
 * clk_beat / clk_pos already stand at the next block's start.
 *   sub bloom   idle: the block bit for bit and no state touched; armed mid-bar while playing: silent until the
 *               next downbeat (beat 4: sample 88200 at 120 BPM), then in from 0 degrees (rising); stopped: at once;
 *               a 50 Hz bass -> 25 Hz, at the bass's level x sub_lvl, locked (near 0 at the input's crossings);
 *               the silence gate (hiss: it fades out, the tracker frozen); disarmed while playing: on until the next
 *               downbeat; the fade out starts at a zero crossing (no step); the SUB kill takes it out
 *   limiter     below the ceiling: the input 8 samples later, bit for bit; a sine at 2 x full scale, a single
 *               impulse at 3 x, a lower ceiling: nothing out above the ceiling; the gain and the hits reported; the
 *               release: back to 1.0 within 0.5 s, not at once; the whole chain with the echo self-oscillating at
 *               1.25 and the siren full on: bounded by the ceiling */
#include <stdint.h>
#define ENABLE_DUB_PREAMP
#define main hostsim_main                               /* SLOOP on the host: as regress.c */
#include "hostsim.c"
#undef main

static int fails;
static void check(const char *what, int ok)
{
    printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

#define SR 44100
static int32_t out_l[SR * 8 + CTL], out_r[SR * 8 + CTL];   /* (+ a block: runs end on a block) */
static uint64_t units;                                  /* transport units since PLAY */

/* one block of the sub bloom on a SUB band of `sub` (Q15 per sample, fn of the absolute sample), l / r silent */
static void sub_run(uint32_t from, uint32_t n_samples, double amp, double hz, int playing)
{
    uint32_t k, i;
    song.playing = (uint8_t)playing;
    for (k = from; k < from + n_samples; k += CTL) {
        int32_t l[CTL] = {0}, r[CTL] = {0};
        for (i = 0; i < CTL; i++)
            dub_xo.sub[i] = (int32_t)lrint(amp * sin(2 * M_PI * hz * (k + i) / SR));
        if (playing) {                                  /* the clock after this block's events: at its end */
            units += (uint64_t)CTL * (uint32_t)song.g[G_BPM];
            clk_beat = (uint32_t)(units / BEAT_U);
            clk_pos = (uint32_t)(units % BEAT_U);
        }
        dub_sub_block(l, r, CTL);
        for (i = 0; i < CTL; i++) {
            out_l[k + i] = l[i];
            out_r[k + i] = r[i];
        }
    }
}
static void sub_fresh(void)
{
    dub_ctl_defaults();
    memset(&dub_sub, 0, sizeof dub_sub);
    dub_xo.g[DUB_SUB] = 32767;
    song.g[G_BPM] = 120;
    song.playing = 0;
    units = 0;
    clk_beat = clk_pos = 0;
}
static uint32_t first_nonzero(uint32_t a, uint32_t b)
{
    uint32_t i;
    for (i = a; i < b; i++)
        if (out_l[i])
            return i;
    return b;
}
static uint32_t crossings(uint32_t a, uint32_t b)
{
    uint32_t i, n = 0;
    for (i = a + 1; i < b; i++)
        n += (out_l[i - 1] < 0) != (out_l[i] < 0);
    return n;
}
static int32_t peak(uint32_t a, uint32_t b)
{
    uint32_t i;
    int32_t p = 0;
    for (i = a; i < b; i++)
        if (abs(out_l[i]) > p)
            p = abs(out_l[i]);
    return p;
}

static void sub_bloom(void)
{
    uint32_t on, i, k;
    /* idle */
    sub_fresh();
    {
        int32_t l[CTL], r[CTL];
        int same = 1;
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)i * 300 - 4000;
        dub_xo.want_sub = 1;
        dub_sub_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            same &= l[i] == (int32_t)i * 300 - 4000 && r[i] == l[i];
        check("sub: not armed -> the block bit for bit, no tracking (state untouched)",
              same && !dub_xo.want_sub && !dub_sub.env && !dub_sub.ph && !dub_sub.per);
    }
    /* armed mid-bar while playing: in on the next downbeat */
    sub_fresh();
    sub_run(0, 29984, 8000, 50, 1);                     /* bar 1 under way (its downbeat at 0 was not armed) */
    dub_ctl.sub_arm = 1;
    sub_run(29984, 176384 - 29984, 8000, 50, 1);        /* (whole blocks: to 176384, just before beat 8) */
    on = first_nonzero(29984, SR * 4);
    printf("  armed at sample 29984 (beat 1.36): first sound at %u (the bar's downbeat: 88200)\n", on);
    check("sub: armed mid-bar -> silent until the next downbeat (beat 4, sample 88200), then in",
          on >= 88200u && on < 88200u + 64u);
    check("sub: in on the downbeat from 0 degrees (the first sound rising)", out_l[on] > 0 && peak(on, on + 8) < 400);
    {
        double rms = 0;
        uint32_t c = crossings(SR * 3, SR * 4);
        for (i = SR * 3; i < SR * 4; i++)
            rms += (double)out_l[i] * out_l[i];
        rms = sqrt(rms / SR);
        printf("  a 50 Hz bass: %u crossings in 1 s (25 Hz: 50), peak %d (8000 x 0.5 = 4000), RMS %.0f\n", c,
               peak(SR * 3, SR * 4), rms);
        check("sub: an octave under (50 Hz -> 25 Hz)", abs((int)c - 50) <= 2);
        check("sub: at the bass's level x sub_lvl (4000 +-10 %)", abs(peak(SR * 3, SR * 4) - 4000) < 400);
        check("sub: the same on both sides", !memcmp(out_l + SR * 3, out_r + SR * 3, SR * sizeof(int32_t)));
    }
    {   /* locked: at the input's rising crossings (every 882 samples) the sub octave is near a zero crossing */
        int32_t worst = 0;
        for (k = 0; k < 40u; k++) {
            uint32_t at = SR * 3 + k * 882u;
            if (abs(out_l[at]) > worst)
                worst = abs(out_l[at]);
        }
        printf("  lock: the bloom at the bass's rising crossings, at most %d (of 4000)\n", worst);
        check("sub: phase-locked to the bass (near 0 at its crossings: < 15 % of its peak)", worst < 600);
    }
    /* disarmed in the last block before beat 8 (176400, its 16th sample): out from there, from a zero crossing */
    dub_ctl.sub_arm = 0;
    sub_run(176384, SR * 8 - 176384 - CTL, 8000, 50, 1);
    {
        uint32_t last = SR * 8;
        int32_t step = 0;
        for (i = SR * 8 - 1; i > SR * 4; i--)
            if (out_l[i]) {
                last = i;
                break;
            }
        for (i = SR * 4 + 1; i < SR * 8; i++)
            if (abs(out_l[i] - out_l[i - 1]) > step)
                step = abs(out_l[i] - out_l[i - 1]);
        printf("  disarmed at 176384: last sound at %u (the downbeat 176400, then a zero crossing); largest step %d\n", last, step);
        check("sub: disarmed while playing -> on until the next downbeat (176400), then out",
              last >= 176400u && last < 176400u + 600u);
        check("sub: the fade out starts at a zero crossing (no step: < 2 x the sine's own 14)", step <= 28);
        check("sub: idle again: no work", !dub_xo.want_sub || (dub_sub_block(out_l, out_r, 0), !dub_xo.want_sub));
    }
    /* stopped: at once */
    sub_fresh();
    dub_ctl.sub_arm = 1;
    sub_run(0, SR, 8000, 50, 0);
    on = first_nonzero(0, SR);
    printf("  stopped, armed at 0: first sound at %u (a period to measure: ~1764)\n", on);
    check("sub: transport stopped -> in at once (as soon as it has a period)", on < 2400u);
    /* the silence gate: the bass stops, only hiss below the gate: it fades out, the tracker frozen */
    {
        uint32_t inc0, per0;
        sub_run(SR, SR, 8000, 50, 0);
        inc0 = dub_sub.inc;
        per0 = dub_sub.per;
        song.playing = 0;
        for (k = SR * 2; k < SR * 4; k += CTL) {        /* hiss: +-30 (~-61 dBFS), random */
            int32_t l[CTL] = {0}, r[CTL] = {0};
            for (i = 0; i < CTL; i++)
                dub_xo.sub[i] = (rand() % 61) - 30;
            dub_sub_block(l, r, CTL);
            for (i = 0; i < CTL; i++)
                out_l[k + i] = l[i];
        }
        printf("  hiss after the bass: sound after 1 s %d; period %u -> %u\n", peak(SR * 3, SR * 4), per0, dub_sub.per);
        check("sub: silence gate: hiss below it -> faded out (0)", peak(SR * 3, SR * 4) == 0);
        check("sub: silence gate: the tracker frozen (the period kept)", dub_sub.per == per0 && dub_sub.inc == inc0);
    }
    /* the SUB kill takes the bloom with it */
    sub_fresh();
    dub_ctl.sub_arm = 1;
    sub_run(0, SR, 8000, 50, 0);
    dub_xo.g[DUB_SUB] = 0;
    sub_run(SR, SR, 8000, 50, 0);
    check("sub: the SUB band killed -> no bloom", peak(SR + 64, SR * 2) == 0);
    dub_xo.g[DUB_SUB] = 32767;
}

/* ------------------------------------------------------------------ limiter --- */
static int32_t in_l[SR * 8 + CTL], in_r[SR * 8 + CTL];
static void lim_run(uint32_t off, uint32_t n)           /* in_l / in_r [off, off + n) -> out_l / out_r, n x CTL */
{
    uint32_t k, i;
    for (k = off; k + CTL <= off + n; k += CTL) {
        int32_t l[CTL], r[CTL];
        for (i = 0; i < CTL; i++)
            l[i] = in_l[k + i], r[i] = in_r[k + i];
        dub_lim_block(l, r, CTL);
        for (i = 0; i < CTL; i++)
            out_l[k + i] = l[i], out_r[k + i] = r[i];
    }
}
static int32_t pk_lr(uint32_t a, uint32_t b)
{
    uint32_t i;
    int32_t p = 0;
    for (i = a; i < b; i++) {
        if (abs(out_l[i]) > p)
            p = abs(out_l[i]);
        if (abs(out_r[i]) > p)
            p = abs(out_r[i]);
    }
    return p;
}

#define BURST (SR / CTL * CTL)                          /* 1 s, whole blocks */
static void limiter(void)
{
    uint32_t i, hits0;
    int32_t pk;
    int same = 1;
    dub_ctl_defaults();
    dub_lim_reset();
    for (i = 0; i < BURST; i++) {                       /* below the ceiling: odd values, both channels */
        in_l[i] = (int32_t)(((i * 2654435761u) >> 17) % 65535u) - 32767;
        in_r[i] = -in_l[i] / 2;
    }
    lim_run(0, BURST);
    for (i = 8; i < BURST; i++)
        same &= out_l[i] == in_l[i - 8] && out_r[i] == in_r[i - 8];
    check("limiter: below the ceiling -> the input 8 samples later, bit for bit", same && dub_tel.lim_gr_q15 == 32767);
    /* a sine at 2 x full scale for 1 s, then at 8000 */
    dub_lim_reset();
    hits0 = dub_tel.lim_hits;
    for (i = 0; i < BURST * 3; i++) {
        double a = i < BURST ? 65534 : 8000;
        in_l[i] = in_r[i] = (int32_t)lrint(a * sin(2 * M_PI * 220 * i / SR));
    }
    lim_run(0, BURST / 2 / CTL * CTL);
    printf("  2 x full scale: gain now %u (Q15; 0.5 = 16384), hits %u\n", dub_tel.lim_gr_q15, dub_tel.lim_hits - hits0);
    check("limiter: the reduction reported (gain ~0.5, hits counted)",
          abs((int)dub_tel.lim_gr_q15 - 16384) < 400 && dub_tel.lim_hits > hits0);
    lim_run(BURST / 2 / CTL * CTL, BURST * 3 - BURST / 2 / CTL * CTL);
    pk = pk_lr(0, BURST * 3);
    printf("  peak out over the burst and after: %d (ceiling 32767)\n", pk);
    check("limiter: a 2 x full-scale sine: nothing out above the ceiling", pk <= 32767);
    {   /* the release: 10 ms and 500 ms after the burst (the output 8 samples behind) */
        double g10 = pk_lr(BURST + 8 + 441, BURST + 8 + 441 + 400) / 8000.0;
        double g500 = pk_lr(BURST + 8 + SR / 2, BURST + 8 + SR / 2 + 400) / 8000.0;
        printf("  release: gain 10 ms after %.2f, 500 ms after %.3f\n", g10, g500);
        check("limiter: the release: not at once (10 ms after < 0.8), back by 0.5 s (> 0.99)", g10 < 0.8 && g500 > 0.99);
    }
    /* one sample at 3 x full scale (and its neighbour on the other side): the look-ahead catches it */
    dub_lim_reset();
    memset(in_l, 0, sizeof in_l);
    memset(in_r, 0, sizeof in_r);
    in_l[1000] = 98000;
    in_r[1003] = -98000;
    lim_run(0, BURST / 4 / CTL * CTL);
    pk = pk_lr(0, BURST / 4 / CTL * CTL);
    printf("  a single sample at 3 x full scale: out at most %d\n", pk);
    check("limiter: a one-sample spike at 3 x full scale: caught by the look-ahead (<= ceiling, > 0.9 x)",
          pk <= 32767 && pk > 29490);
    /* a lower ceiling */
    dub_lim_reset();
    dub_ctl.out_q15 = 16384;
    for (i = 0; i < BURST; i++)
        in_l[i] = in_r[i] = (int32_t)lrint(30000 * sin(2 * M_PI * 1000 * i / SR));
    lim_run(0, BURST);
    pk = pk_lr(0, BURST);
    check("limiter: ceiling at 16384 (out_q15) -> nothing above it", pk <= 16384 && pk > 15000);
    dub_ctl.out_q15 = 32767;
}

/* the whole chain: the echo running away at 1.25 with the siren full on: bounded by the ceiling */
static void chain(void)
{
    uint32_t b, i;
    int32_t pk = 0;
    dub_ctl_defaults();
    song.g[G_BPM] = 120;
    dub_chain.w = 0;
    dub_chain.cold = 1;
    song.g[G_DTIME] = 4;                                /* (Phase 5: SLOOP's DLY / REV pages: the tape echo, 8T) */
    song.g[G_DFDBK] = 125;                              /* 1.25: running away */
    song.g[G_DMIX] = 127;
    song.g[G_DCOLOR] = 127;
    song.g[G_RSIZE] = 127;
    dub_ctl.echo_send_q15 = 32767;
    dub_ctl.rev_send_q15 = 32767;
    dub_ctl.siren_gate = 1;
    dub_ctl.siren_lvl_q15 = 32767;
    dub_ctl.siren_depth_q8 = 12 << 8;
    dub_ctl.drive_q12 = DUB_DRIVE_MAX;
    for (b = 0; b < SR * 6 / CTL; b++) {
        int32_t l[CTL], r[CTL];
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)lrint(30000 * sin(2 * M_PI * 55 * (b * CTL + i) / SR));
        dub_mix_intercept(l, r, l, r, CTL);              /* (the sends too: the bus itself, full) */
        for (i = 0; i < CTL; i++) {
            if (abs(l[i]) > pk)
                pk = abs(l[i]);
            if (abs(r[i]) > pk)
                pk = abs(r[i]);
        }
    }
    printf("  the chain flat out (drive x8, echo 1.25 into the spring, siren full): peak %d, gain %u\n", pk,
           dub_tel.lim_gr_q15);
    check("chain: everything at once, running away: the output bounded by the ceiling", pk <= 32767);
    dub_ctl.siren_gate = 0;
}

int main(void)
{
    srand(1);
    song.g[G_BPM] = 120;
    sub_bloom();
    limiter();
    chain();
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
