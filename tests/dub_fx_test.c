/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's effects (firmware/src/dub_preamp/dub_effects.c), its MIDI map (dub_midi.c) and the
 * mixer intercept with the USB audio (dub_mixer_intercept.c), on SLOOP's host build (hostsim.c, USB audio on):
 *   saturation  unity for small signals, tanh at the knee, bounded at 4 x full scale, odd (symmetric), drive x8
 *   echo        the repeat lands at the set time (ms, and a tempo division); feedback 0.5 halves each repeat;
 *               0.9 rings and ends at exactly 0; 1.25 runs away into the tanh but stays bounded and keeps sounding;
 *               FREEZE keeps the loop going
 *               (no decay, no new input); THROW takes the dry out and sends it all; ECHO>REV stops the repeats
 *               coming round (they go to the spring); a 15 kHz input does not fold back (< -50 dB); the 22.05 kHz
 *               return has no image of a 3 kHz repeat (< -50 dB at 19.05 kHz)
 *   spring      a crash rings, stays bounded and dies away
 *   siren       A4 at 440 Hz; the gate's envelope (on, off, silent); a square LFO an octave deep alternates 440 / 880
 *               at its rate; on the clock, a 1/4 at 120 BPM; the ping-pong pan moves the tone left and right
 *   MIDI        the preamp's channel only; kills and punch slots in dub_live at once; the knobs and switches through
 *               the queue into dub_ctl (CC 7, 20, 21, 22, 23, 50..53); CC 45 crashes; another channel is SLOOP's
 *   intercept   BYPASS + no USB: the mix bit for bit; the USB audio summed (and with BYPASS); fader 0: the mix */
#include <stdint.h>
#define FELUCCA_UAC 1
#define ENABLE_DUB_PREAMP
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#define main hostsim_main                               /* SLOOP on the host: as regress.c */
#include "hostsim.c"
#undef main

static int fails;
static void check(const char *what, int ok)
{
    printf("%-74s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---- helpers: the space block on a mono input (L = R), the wet part out (the dry is passed through, 1.0) */
#define SR 44100
static void space_run(const int32_t *in, int32_t *out_l, int32_t *out_r, uint32_t n)
{
    uint32_t k, i;
    for (k = 0; k < n; k += CTL) {
        int32_t l[CTL], r[CTL];
        uint32_t m = n - k < CTL ? n - k : CTL;
        for (i = 0; i < m; i++)
            l[i] = r[i] = in ? in[k + i] : 0;
        dub_space_block(l, r, m);
        for (i = 0; i < m; i++) {
            if (out_l)
                out_l[k + i] = l[i];
            if (out_r)
                out_r[k + i] = r[i];
        }
    }
}
static double rms(const int32_t *x, uint32_t a, uint32_t b)
{
    double e = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        e += (double)x[i] * x[i];
    return sqrt(e / (b - a));
}
static double goertzel(const int32_t *x, uint32_t a, uint32_t b, double f)   /* amplitude at f */
{
    double w = 2 * M_PI * f / SR, c = 2 * cos(w), s1 = 0, s2 = 0;
    uint32_t i;
    for (i = a; i < b; i++) {
        double s0 = x[i] * (0.5 - 0.5 * cos(2 * M_PI * (i - a) / (b - a))) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / ((b - a) * 0.5);
}
static void fx_defaults(void)
{
    dub_ctl_defaults();
    memset(&dub_fx, 0, sizeof dub_fx);
    dub_fx.dry = 32767;
    dub_fx.d_q8 = 8000 << 8;
    dub_fx.lfsr = 0xACE1u;
    memset(dub_line_mem, 0, sizeof dub_line_mem);
    dub_fx.crash_c = dub_ctl.ev_crash;                  /* (the events already seen: no crash left over) */
    dub_fx.crash_l = dub_live.ev_crash;
    dub_fx.sev_c = dub_ctl.ev_siren;
    dub_fx.sev_l = dub_live.ev_siren;
    dub_live.punch = DUB_PX_NONE;
    dub_ctl.echo_sync = 0;
    dub_ctl.echo_send_q15 = 32767;
    dub_ctl.echo_ret_q15 = 32767;
    dub_ctl.echo_fb_q12 = 0;
    dub_ctl.rev_send_q15 = 0;
    song.g[G_BPM] = 120;
    song.playing = 0;
}

static int32_t buf_in[SR * 12], buf_l[SR * 12], buf_r[SR * 12];

static void saturation(void)
{
    int32_t l[CTL], r[CTL];
    uint32_t i;
    int ok = 1;
    dub_ctl_defaults();
    for (i = 0; i < CTL; i++)
        l[i] = (int32_t)i * 4096 - 65536, r[i] = -l[i];
    dub_sat_block(l, r, CTL);
    for (i = 0; i < CTL; i++) {
        double x = ((int32_t)i * 4096 - 65536) / 32768.0, want = 32767 * tanh(x > 2 ? 2 : x < -2 ? -2 : x);
        ok &= fabs(l[i] - want) < 40 && r[i] == -l[i];
    }
    check("saturation: tanh at unity drive (-2 .. +2 x full scale), odd", ok);
    l[0] = 1000, r[0] = 300000, l[1] = -1000000, r[1] = 10;
    dub_sat_block(l, r, 2);
    check("saturation: small signals pass (1000 -> 1000 +-1 %), huge ones held at tanh(2) (-0.3 dBFS)",
          abs(l[0] - 1000) <= 10 && abs(r[0] - 31588) <= 2 && abs(l[1] + 31588) <= 2 && abs(r[1] - 10) <= 1);
    dub_ctl.drive_q12 = DUB_DRIVE_MAX;
    l[0] = r[0] = 1000;
    dub_sat_block(l, r, 1);
    check("saturation: drive x8 (1000 -> ~7700, tanh(0.24))", abs(l[0] - (int32_t)(32767 * tanh(8000 / 32768.0))) < 60);
}

static uint32_t argmax_abs(const int32_t *x, uint32_t a, uint32_t b)
{
    uint32_t i, k = a;
    for (i = a; i < b; i++)
        if (abs(x[i]) > abs(x[k]))
            k = i;
    return k;
}

static void echo(void)
{
    uint32_t i, at, want;
    double r1, r2;
    /* the time: an impulse after 1 s of settling, the repeat 250 ms later */
    fx_defaults();
    dub_ctl.echo_ms = 250;
    memset(buf_in, 0, sizeof buf_in);
    buf_in[SR] = 30000;
    space_run(buf_in, buf_l, buf_r, SR * 2);
    for (i = 0; i < SR * 2; i++)
        buf_l[i] -= buf_in[i];                          /* the wet part */
    at = argmax_abs(buf_l, SR + 100, SR * 2);
    want = SR + 250u * 441u / 10u;
    printf("  250 ms: the repeat at +%u samples (want %u)\n", at - SR, want - SR);
    check("echo: 250 ms lands at 11025 samples (+-3)", abs((int)at - (int)want) <= 3);
    /* a tempo division: 1/8 dotted at 120 BPM = 375 ms */
    fx_defaults();
    dub_ctl.echo_sync = 1;
    dub_ctl.echo_div = 6;                               /* N_DLY: 1/8 dotted */
    space_run(buf_in, buf_l, buf_r, SR * 2);
    for (i = 0; i < SR * 2; i++)
        buf_l[i] -= buf_in[i];
    at = argmax_abs(buf_l, SR + 100, SR * 2);
    want = SR + dly_samples(6);
    printf("  1/8 dotted at 120: +%u samples (want %u)\n", at - SR, want - SR);
    check("echo: on the tempo (1/8 dotted at 120 BPM) within 3 samples", abs((int)at - (int)want) <= 3);
    /* feedback 0.5: a 500 Hz burst, each repeat half the last */
    fx_defaults();
    dub_ctl.echo_ms = 200;
    dub_ctl.echo_fb_q12 = 2048;
    memset(buf_in, 0, sizeof buf_in);
    for (i = SR; i < SR + 2205; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 500 * i / SR));
    space_run(buf_in, buf_l, buf_r, SR * 3);
    for (i = 0; i < SR * 3; i++)
        buf_l[i] -= buf_in[i];
    r1 = rms(buf_l, SR + 8820 + 400, SR + 8820 + 1800);
    r2 = rms(buf_l, SR + 17640 + 400, SR + 17640 + 1800);
    printf("  feedback 0.5: repeat 1 %.0f, repeat 2 %.0f (ratio %.3f)\n", r1, r2, r2 / r1);
    check("echo: feedback 0.5 halves each repeat (0.45 .. 0.52)", r2 / r1 > 0.45 && r2 / r1 < 0.52);
    /* 1.25: runs away, bounded, still sounding after 10 s */
    fx_defaults();
    dub_ctl.echo_ms = 300;
    dub_ctl.echo_fb_q12 = DUB_FB_MAX;
    space_run(buf_in, buf_l, buf_r, SR * 12);
    {
        int32_t pk = 0;
        for (i = 0; i < SR * 12; i++)
            if (abs(buf_l[i] - buf_in[i]) > pk)
                pk = abs(buf_l[i] - buf_in[i]);
        r1 = rms(buf_l, SR * 11, SR * 12);
        printf("  feedback 1.25: peak %d, level after 11 s %.0f\n", pk, r1);
        check("echo: feedback 1.25 self-oscillates, bounded (< 40000), still loud", pk < 40000 && r1 > 4000);
    }
    /* 0.9: a long tail that still ends at exactly 0 (no stuck value going round) */
    fx_defaults();
    dub_ctl.echo_ms = 150;
    dub_ctl.echo_fb_q12 = 3686;
    memset(buf_in, 0, sizeof buf_in);
    for (i = SR; i < SR + 2205; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 500 * i / SR));
    space_run(buf_in, buf_l, NULL, SR * 12);
    for (i = 0; i < SR * 12; i++)
        buf_l[i] -= buf_in[i];
    printf("  feedback 0.9: level at 2 s %.0f, after 11 s %.2f\n", rms(buf_l, SR * 2, SR * 3), rms(buf_l, SR * 11, SR * 12));
    check("echo: feedback 0.9 rings on, then dies to exactly 0", rms(buf_l, SR * 2, SR * 3) > 500 && rms(buf_l, SR * 11, SR * 12) == 0);
    /* FREEZE: a burst in, frozen: the same level 5 s later, nothing new gets in */
    fx_defaults();
    dub_ctl.echo_ms = 300;
    dub_ctl.echo_fb_q12 = 2458;
    memset(buf_in, 0, sizeof buf_in);
    for (i = SR; i < SR + 4410; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 500 * i / SR));
    space_run(buf_in, buf_l, NULL, SR + 6000);
    dub_live.punch = DUB_PX_ECHO_FREEZE;
    for (i = SR + 6000; i < SR * 8; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 3000 * i / SR));   /* a new sound: must not enter */
    space_run(buf_in + SR + 6000, buf_l + SR + 6000, NULL, SR * 7 - 6000);
    for (i = SR + 6000; i < SR * 8; i++)
        buf_l[i] -= buf_in[i];
    r1 = rms(buf_l, SR * 2, SR * 2 + 13230);
    r2 = rms(buf_l, SR * 7, SR * 7 + 13230);
    printf("  FREEZE: %.0f at 2 s, %.0f at 7 s; the new 3 kHz in it %.1f\n", r1, r2, goertzel(buf_l, SR * 7, SR * 7 + 13230, 3000));
    check("echo: FREEZE holds the loop (level at 7 s within 0.5 dB of 2 s)", r1 > 1000 && fabs(20 * log10(r2 / r1)) < 0.5);
    check("echo: FREEZE takes no new input (the new 3 kHz < -60 dB)", goertzel(buf_l, SR * 7, SR * 7 + 13230, 3000) < 16);
    dub_live.punch = DUB_PX_NONE;
    /* THROW: the dry gone, the whole bus to the echo */
    fx_defaults();
    dub_ctl.echo_send_q15 = 0;                          /* (the throw sends all whatever the send) */
    dub_ctl.echo_ms = 250;
    dub_live.punch = DUB_PX_ECHO_THROW;
    for (i = 0; i < SR * 2; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 1000 * i / SR));
    space_run(buf_in, buf_l, NULL, SR * 2);
    r1 = rms(buf_l, 200, 10000);
    r2 = rms(buf_l, 12000, SR);
    printf("  THROW: before the echo returns %.0f, after %.0f\n", r1, r2);
    check("echo: THROW: the dry out at once (< 1 %), the echo of all of it after", r1 < 113 && r2 > 9000);
    dub_live.punch = DUB_PX_NONE;
    /* ECHO>REV: the repeats stop coming round */
    {
        double off2, on2;
        uint32_t pass;
        for (pass = 0; pass < 2u; pass++) {
            fx_defaults();
            dub_ctl.echo_ms = 200;
            dub_ctl.echo_fb_q12 = 3276;                 /* 0.8 */
            dub_ctl.echo_to_rev = (uint8_t)pass;
            memset(buf_in, 0, sizeof buf_in);
            for (i = SR; i < SR + 2205; i++)
                buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 500 * i / SR));
            space_run(buf_in, buf_l, NULL, SR * 3);
            for (i = 0; i < SR * 3; i++)
                buf_l[i] -= buf_in[i];
            if (pass)
                on2 = goertzel(buf_l, SR + 17640 + 400, SR + 17640 + 1800, 500);
            else
                off2 = goertzel(buf_l, SR + 17640 + 400, SR + 17640 + 1800, 500);
        }
        printf("  ECHO>REV: repeat 2 at 500 Hz: %.0f off, %.0f on\n", off2, on2);
        check("echo: ECHO>REV: the second repeat is gone from the line (< 10 %)", on2 < off2 * 0.1);
    }
    /* no aliasing: 15 kHz in, nothing comes back */
    fx_defaults();
    dub_ctl.echo_ms = 100;
    for (i = 0; i < SR * 2; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 15000 * i / SR));
    space_run(buf_in, buf_l, NULL, SR * 2);
    for (i = 0; i < SR * 2; i++)
        buf_l[i] -= buf_in[i];
    r1 = rms(buf_l, SR, SR * 2);
    printf("  15 kHz in: the echo %.1f (%.1f dB re the input)\n", r1, 20 * log10(r1 / (16000 / M_SQRT2) + 1e-12));
    check("echo: 15 kHz does not fold back (< -50 dB)", r1 < 16000 / M_SQRT2 * 0.00316);
    /* no image: a 3 kHz repeat, nothing at 22050 - 3000 */
    fx_defaults();
    dub_ctl.echo_ms = 100;
    for (i = 0; i < SR * 2; i++)
        buf_in[i] = (int32_t)(16000 * sin(2 * M_PI * 3000 * i / SR));
    space_run(buf_in, buf_l, NULL, SR * 2);
    for (i = 0; i < SR * 2; i++)
        buf_l[i] -= buf_in[i];
    {
        double a3 = goertzel(buf_l, SR, SR * 2, 3000), ai = goertzel(buf_l, SR, SR * 2, 19050);
        printf("  3 kHz repeat %.0f, image at 19.05 kHz %.2f (%.1f dB)\n", a3, ai, 20 * log10(ai / a3 + 1e-12));
        check("echo: the 22.05 kHz return has no image (< -50 dB)", ai < a3 * 0.00316);
    }
}

static void spring(void)
{
    uint32_t i;
    int32_t pk = 0;
    double early, late;
    fx_defaults();
    dub_ctl.echo_send_q15 = 0;
    dub_ctl.echo_ret_q15 = 0;
    dub_ctl.rev_decay_q15 = 26214;                      /* 0.8 */
    memset(buf_in, 0, sizeof buf_in);
    space_run(buf_in, buf_l, NULL, SR / 2);
    dub_ev_post(&dub_ctl.ev_crash);
    space_run(buf_in, buf_l, NULL, SR * 6);
    for (i = 0; i < SR * 6; i++)
        if (abs(buf_l[i]) > pk)
            pk = abs(buf_l[i]);
    early = rms(buf_l, 2000, 8000);
    late = rms(buf_l, SR * 5, SR * 6);
    printf("  crash: peak %d, early %.0f, after 5 s %.2f\n", pk, early, late);
    check("spring: a crash rings (peak > -15 dBFS), bounded (< 40000)", pk > 5800 && early > 300 && pk < 40000);
    check("spring: and dies away to 0 (after 5 s)", late == 0);
}

static uint32_t crossings(const int32_t *x, uint32_t a, uint32_t b)
{
    uint32_t i, n = 0;
    for (i = a + 1; i < b; i++)
        n += (x[i - 1] < 0) != (x[i] < 0);
    return n;
}

static void siren(void)
{
    uint32_t i, c1, c2;
    double on, off;
    fx_defaults();
    dub_ctl.echo_send_q15 = 0;
    dub_ctl.echo_ret_q15 = 0;
    dub_ctl.siren_note_q8 = 69 << 8;                    /* A4 */
    dub_ctl.siren_depth_q8 = 0;
    dub_ctl.siren_lvl_q15 = 16384;
    dub_ctl.siren_gate = 1;
    space_run(NULL, buf_l, buf_r, SR * 2);
    c1 = crossings(buf_l, SR, SR * 2);
    on = rms(buf_l, SR, SR * 2);
    printf("  A4: %u crossings in 1 s (880), level %.0f (centre: 16384 x 0.707 / sqrt 2 = %.0f)\n", c1, on, 16384 * 0.7071 / M_SQRT2);
    check("siren: A4 at 440 Hz", abs((int)c1 - 880) <= 2);
    check("siren: its level (siren_lvl, centred: -3 dB a side)", fabs(on - 16384 * 0.7071 / M_SQRT2) < 300);
    dub_ctl.siren_gate = 0;
    space_run(NULL, buf_l, buf_r, SR);
    off = rms(buf_l, SR / 5, SR);
    check("siren: the gate off: silent after the release", off == 0);
    /* a square LFO an octave deep at 2 Hz: 440 / 880 alternating every 0.25 s */
    dub_ctl.siren_gate = 1;
    dub_ctl.siren_wave = DUB_WAVE_SQUARE;
    dub_ctl.siren_depth_q8 = 12 << 8;
    dub_ctl.siren_rate_ch = 200;
    dub_ctl.siren_note_q8 = 75 << 8;                    /* the LFO's +-1: 81 (880 Hz) and 69 (440 Hz) */
    dub_ctl.siren_depth_q8 = 6 << 8;
    dub_fx.lph = 0;                                     /* (the LFO from its start: the windows sit inside halves) */
    space_run(NULL, buf_l, buf_r, SR * 2);
    c1 = crossings(buf_l, SR + 2000, SR + 2000 + 8000);
    c2 = crossings(buf_l, SR + 2000 + 11025, SR + 2000 + 11025 + 8000);
    printf("  square LFO 2 Hz, +-6 semitones: %u then %u crossings in 8000 samples (880 Hz: 319, 440 Hz: 160)\n", c1, c2);
    check("siren: a square LFO alternates two pitches an octave apart, at its rate",
          (abs((int)c1 - 160) <= 4 && abs((int)c2 - 319) <= 4) || (abs((int)c2 - 160) <= 4 && abs((int)c1 - 319) <= 4));
    /* on the clock: a 1/4 at 120 BPM = 2 Hz as well */
    dub_ctl.siren_sync = 1;
    dub_ctl.siren_div = 0;                              /* N_DIV: 1/4 */
    dub_fx.lph = 0;
    space_run(NULL, buf_l, buf_r, SR * 2);
    c1 = crossings(buf_l, SR + 2000, SR + 2000 + 8000);
    c2 = crossings(buf_l, SR + 2000 + 11025, SR + 2000 + 11025 + 8000);
    check("siren: SIREN CLK, a 1/4 at 120 BPM: the same 2 Hz alternation",
          (abs((int)c1 - 160) <= 4 && abs((int)c2 - 319) <= 4) || (abs((int)c2 - 160) <= 4 && abs((int)c1 - 319) <= 4));
    dub_ctl.siren_sync = 0;
    /* the ping-pong pan at 1 Hz: left, then right */
    dub_ctl.siren_wave = DUB_WAVE_SINE;
    dub_ctl.siren_depth_q8 = 0;
    dub_ctl.pan_on = 1;
    dub_ctl.pan_rate_ch = 100;
    space_run(NULL, buf_l, buf_r, SR * 2);
    {
        double l0 = rms(buf_l, SR, SR + 4410), r0 = rms(buf_r, SR, SR + 4410);
        double l1 = rms(buf_l, SR + 22050, SR + 22050 + 4410), r1 = rms(buf_r, SR + 22050, SR + 22050 + 4410);
        printf("  pan: L/R %.0f/%.0f, half a cycle later %.0f/%.0f\n", l0, r0, l1, r1);
        check("siren: the ping-pong pan moves it from one side to the other", (l0 > 2 * r0 && r1 > 2 * l1) || (r0 > 2 * l0 && l1 > 2 * r1));
    }
    dub_ctl.siren_gate = 0;
}

static void midi(void)
{
    uint32_t c0;
#define CC(ch, cc, v) ((uint32_t)0x0B | (0xB0u | ((ch) - 1u)) << 8 | (uint32_t)(cc) << 16 | (uint32_t)(v) << 24)
    dub_ctl_defaults();
    dub_live.kill = 0;
    dub_live.punch = DUB_PX_NONE;
    dub_mq.r = dub_mq.w;
    check("MIDI: channel 11 is the preamp's", dub_midi_take(CC(11, 30, 127)) && dub_live.kill == DUB_KILL_SUB);
    dub_midi_take(CC(11, 33, 100));
    dub_midi_take(CC(11, 30, 0));
    check("MIDI: CC 30..33 kill and open at once (dub_live)", dub_live.kill == DUB_KILL_TREBLE);
    check("MIDI: another channel is SLOOP's", !dub_midi_take(CC(10, 30, 127)) && dub_live.kill == DUB_KILL_TREBLE);
    check("MIDI: a note on the preamp's channel is dropped",
          dub_midi_take(0x09u | (0x90u | 10u) << 8 | 60u << 16 | 100u << 24));
    dub_midi_take(CC(11, 41, 127));
    check("MIDI: CC 41 = ECHO FREEZE on", dub_live.punch == DUB_PX_ECHO_FREEZE);
    dub_midi_take(CC(11, 40, 0));
    check("MIDI: another slot's release leaves it", dub_live.punch == DUB_PX_ECHO_FREEZE);
    dub_midi_take(CC(11, 41, 0));
    check("MIDI: its own release ends it", dub_live.punch == DUB_PX_NONE);
    c0 = dub_live.ev_crash;
    dub_midi_take(CC(11, 45, 127));
    dub_midi_take(CC(11, 45, 0));
    check("MIDI: CC 45 crashes once", dub_live.ev_crash == c0 + 1u);
    dub_midi_take(CC(11, 7, 127));
    dub_midi_take(CC(11, 20, 64));
    dub_midi_take(CC(11, 21, 127));
    dub_midi_take(CC(11, 22, 127));
    dub_midi_take(CC(11, 23, 0));
    dub_midi_take(CC(11, 50, 127));
    dub_midi_take(CC(11, 51, 127));
    dub_midi_take(CC(11, 52, 127));
    dub_midi_take(CC(11, 53, 127));
    check("MIDI: the knobs wait for the main loop (dub_ctl untouched in the ISR)", dub_ctl.usb_in_q15 == 32767 &&
          dub_ctl.tubby_step == 0 && !dub_ctl.bypass);
    dub_mq_apply();
    check("MIDI: CC 7 USB fader, 20 big knob (64 -> step 5, in), 21 drive x8, 22 feedback 1.25, 23 siren C1",
          dub_ctl.usb_in_q15 == 32767 && dub_ctl.tubby_step == 5 && dub_ctl.tubby_on && dub_ctl.drive_q12 == DUB_DRIVE_MAX &&
          dub_ctl.echo_fb_q12 == DUB_FB_MAX && dub_ctl.siren_note_q8 == 24 << 8);
    check("MIDI: CC 50..53 ECHO>REV, BYPASS, SIREN CLK, SUB BLOOM on",
          dub_ctl.echo_to_rev && dub_ctl.bypass && dub_ctl.siren_sync && dub_ctl.sub_arm);
    dub_midi_take(CC(11, 51, 0));
    dub_midi_take(CC(11, 20, 127));
    dub_mq_apply();
    check("MIDI: switches follow the value (51 at 0: BYPASS off); 20 at 127: step 10",
          !dub_ctl.bypass && dub_ctl.tubby_step == 10);
    dub_ctl.midi_ch = 0;
    check("MIDI: channel OFF: nothing is taken", !dub_midi_take(CC(11, 30, 127)));
    dub_ctl.midi_ch = 11;
#undef CC
}

/* ------------------------------------------------------------ mixer intercept --- */
static void intercept(void)
{
    int32_t ml[CTL], mr[CTL];
    uint32_t i, k, same = 1, sum_ok = 1, blocks;
    dub_ctl_defaults();
    dub_ctl.bypass = 1;                                 /* (the chain's own tests: above, dub_xover_test.c) */
    dub_chain.w = 0;
    dub_uac_stream(0);                                  /* no USB stream, BYPASS: the mix goes through untouched */
    for (k = 0; k < 4u; k++) {
        for (i = 0; i < CTL; i++)
            ml[i] = (int32_t)(i * 1000u) - 9000, mr[i] = 7000 - (int32_t)(i * 333u);
        dub_mix_intercept(ml, mr, CTL);
        for (i = 0; i < CTL; i++)
            same &= ml[i] == (int32_t)(i * 1000u) - 9000 && mr[i] == 7000 - (int32_t)(i * 333u);
    }
    check("intercept: no USB stream, BYPASS -> the SLOOP mix bit for bit", same);
    dub_uac_stream(1);                                  /* the computer plays a constant: L 8000, R -4000 */
    dub_ctl.usb_in_q15 = 32767;
    for (k = 0; k < 200u; k++) {
        uint32_t w = dub_ua_w;
        for (i = 0; i < 45u && dub_ua_w - dub_ua_r < DUB_UA_N - 64u; i++)
            dub_ua_ring[(w + i) & (DUB_UA_N - 1u)] = (uint16_t)8000 | (uint32_t)(uint16_t)-4000 << 16;
        dub_ua_w = w + i;
        dub_uac.live = 1;
        for (i = 0; i < CTL; i++)
            ml[i] = 1000, mr[i] = 1000;
        dub_mix_intercept(ml, mr, CTL);
    }
    for (i = 0; i < CTL; i++)                           /* (the fader ramp settles within 1 % of unity) */
        sum_ok &= abs(ml[i] - (1000 + 8000)) <= 90 && abs(mr[i] - (1000 - 4000)) <= 90;
    check("intercept: the bus = the mix + the USB audio (fader at 1.0, BYPASS)", sum_ok);
    blocks = dub_tel.blocks;
    dub_mix_intercept(ml, mr, CTL);
    check("intercept: one block counted", dub_tel.blocks == blocks + 1u);
    dub_ctl.usb_in_q15 = 0;                             /* the fader down: the USB part fades to nothing */
    for (k = 0; k < 100u; k++) {
        for (i = 0; i < CTL; i++)
            ml[i] = 1000, mr[i] = 1000;
        dub_mix_intercept(ml, mr, CTL);
        dub_ua_r = dub_ua_w - DUB_UA_TARGET;            /* (keep the ring fed: no underrun) */
    }
    check("intercept: fader at 0 -> the mix alone", abs(ml[CTL - 1u] - 1000) <= 2 && abs(mr[CTL - 1u] - 1000) <= 2);
}

int main(void)
{
    song.g[G_BPM] = 120;
    saturation();
    echo();
    spring();
    siren();
    midi();
    intercept();
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
