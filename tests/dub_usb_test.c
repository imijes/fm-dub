/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's USB playback (firmware/src/dub_preamp/dub_usb_audio.c, ENABLE_DUB_PREAMP):
 *   descriptors  the configuration parsed as a host does (with CDC: -DT_CDC=1, without: 0, CDC built in with
 *                USB SERIAL OFF: 2): lengths, wTotalLength, interface numbers and count, the AC header's
 *                collection, the AC class-specific total, terminals 1..4, IF 3 alt 0 / 1 with one isochronous
 *                adaptive OUT endpoint 0x04 of DUB_UA_MAXP, endpoint addresses unique
 *   ring         packets of 44 / 45 frames at the host's 44.1 kHz (1 ms, jittered as TIMER5 takes them) against
 *                renders of HALF_FRAMES frames at the FM-1's clock, late by 7..85 % of a half: the reader starts
 *                once, never underruns or overruns, centres the fill on its target (taken where its loop takes it:
 *                after a render's reads; the swing: a render and a packet), settles its step on the
 *                clock ratio, and plays a 440 Hz sine back as a sine (error < 0.1 % of full scale, the fader
 *                applied); the same with the FM-1 slower than the host; then the host stops (a fade to 0)
 *                and starts again (a new start).
 * The SIE is never touched: the producer side writes the ring as dub_uac_service does. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifndef HALF_FRAMES
#error "-DHALF_FRAMES=n (src/core.h; run_tests.sh passes it)"
#endif
#define ENABLE_DUB_PREAMP
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 0
#define FELUCCA_CDC (T_CDC != 0)
#define FELUCCA_UAC 1
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"   /* SIE register macros (never touched here) */
#pragma GCC diagnostic ignored "-Wunused-function"       /* usb.c's poll, start, MIDI out: not called here */
#define __attribute__(x)                                /* (.pool and the like: as hostsim.c) */
#include "../firmware/src/dub_preamp/dub_shared_state.h"
#include "../firmware/src/usb.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---------------------------------------------------------------- descriptors --- */
static void descriptors(void)
{
    const uint8_t *d = CFG_DESC;
    uint32_t len = sizeof CFG_DESC, i, cur_if = 0xFF, cur_alt = 0, ac_cs = 0, in_ac = 0, nif = 0;
    uint32_t seen_if = 0, eps_in = 0, eps_out = 0, terms = 0, play_eps = 0, bad = 0;
    uint8_t coll[8] = {0}, ncoll = 0;
#if FELUCCA_CDC
    if (T_CDC == 2) {                                   /* the menu's USB SERIAL OFF: what the host gets */
        d = cfg_plain_get();
        len = CFG_PLAIN_LEN;
    }
#endif
    check("wTotalLength = the bytes", (uint32_t)(d[2] | d[3] << 8) == len);
    for (i = 0; i < len;) {
        uint32_t l = d[i], t = d[i + 1];
        if (l < 2u || i + l > len) {
            bad++;
            break;
        }
        if (t == 4) {                                   /* interface */
            cur_if = d[i + 2];
            cur_alt = d[i + 3];
            in_ac = d[i + 5] == 1 && d[i + 6] == 1;
            if (!((seen_if >> cur_if) & 1u))
                nif++;
            seen_if |= 1u << cur_if;
            if (cur_if == DUB_AS_IF && (d[i + 5] != 1 || d[i + 6] != 2 || d[i + 4] != cur_alt))
                bad++;                                  /* audio streaming, alt 0: 0 EP, alt 1: 1 */
        } else if (t == 0x24 && in_ac) {               /* AC class-specific */
            ac_cs += l;
            if (d[i + 2] == 1) {
                uint32_t k;
                ncoll = d[i + 7];
                for (k = 0; k < ncoll && k < 8u; k++)
                    coll[k] = d[i + 8 + k];
                if (l != 8u + ncoll)
                    bad++;
            } else if (d[i + 2] == 2 || d[i + 2] == 3) {
                terms |= 1u << d[i + 3];
            }
        } else if (t == 5) {                            /* endpoint */
            uint32_t a = d[i + 2], bit = 1u << (a & 15u);
            if (a & 0x80u) {
                if (eps_in & bit)
                    bad++;
                eps_in |= bit;
            } else {
                if (eps_out & bit)
                    bad++;
                eps_out |= bit;
            }
            if (cur_if == DUB_AS_IF) {
                play_eps++;
                check("IF 3 alt 1: EP 0x04 OUT, isochronous adaptive, 1 ms",
                      cur_alt == 1u && a == 0x04u && d[i + 3] == 0x09u && d[i + 6] == 1u);
                check("IF 3: max packet DUB_UA_MAXP (48 frames)", (uint32_t)(d[i + 4] | d[i + 5] << 8) == DUB_UA_MAXP);
            }
        }
        i += l;
    }
    check("descriptors chain to the end", !bad && i == len);
    check("bNumInterfaces = the interfaces", d[4] == nif);
    check("AC header: total = its class-specific bytes", ac_cs == DUB_AC_TOTAL);
    check("AC header: collection = MIDI, recording, playback",
          ncoll == 3u && coll[0] == 1u && coll[1] == UAC_AS_IF && coll[2] == DUB_AS_IF);
    check("terminals 1..4", (terms & 0x1Eu) == 0x1Eu);
    check("one playback endpoint", play_eps == 1u);
    check("EP4 both ways: 0x84 recording, 0x04 playback", (eps_in & 0x10u) && (eps_out & 0x10u));
    check("CDC after the playback", T_CDC != 1 || CDC_IF == 4);
}

/* -------------------------------------------------------------------- ring --- */
static uint32_t host_frame;                             /* frames the host has sent */
static void host_packet(uint32_t n, double amp)          /* as dub_uac_service, without the SIE */
{
    uint32_t w = dub_ua_w, room = DUB_UA_N - (w - dub_ua_r), i;
    if (n > room) {
        dub_uac.overruns += n - room;
        n = room;
    }
    for (i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * 440.0 * (double)(host_frame + i) / 44100.0;
        int16_t v = (int16_t)lrint(amp * sin(ph)), c = (int16_t)lrint(amp * cos(ph));
        dub_ua_ring[(w + i) & (DUB_UA_N - 1u)] = (uint16_t)v | (uint32_t)(uint16_t)c << 16;
    }
    host_frame += n;
    RING_PUBLISH();
    dub_ua_w = w + n;
    dub_uac.pkts++;
    dub_uac.idle = 0;
    dub_uac.live = 1;
}

/* seconds of playback with the FM-1 at fs_dev; returns the largest error (Q15 units) once settled. The fill is
 * taken where the reader's loop takes it: after a render's reads (*fill_mean: its average, settled) */
static double fill_mean;
static double run(double fs_dev, double secs, double amp, uint32_t *fill_lo, uint32_t *fill_hi)
{
    double fill_sum = 0;
    uint32_t fill_n = 0;
    double t = 0, next_pkt = 0, next_half = 0.0003, half = HALF_FRAMES / fs_dev, maxerr = 0;
    uint32_t acc = 0, halves = 0, k;
    *fill_lo = 0xFFFFFFFFu;
    *fill_hi = 0;
    while (t < secs) {
        if (next_pkt <= next_half) {                    /* a 1 ms packet, taken by TIMER5 0..0.5 ms later */
            uint32_t n = 44u;
            acc += 100u;
            if (acc >= 1000u) {
                acc -= 1000u;
                n = 45u;
            }
            t = next_pkt;
            host_packet(n, amp);
            next_pkt += 0.001;
            continue;
        }
        t = next_half;                                  /* a render: late by 7..85 % of a half */
        {
            double late = half * (0.07 + 0.78 * (rand() / (double)RAND_MAX));
            /* the packets due before the render reaches the reads (TIMER5 nests in the render) */
            while (next_pkt <= next_half + late) {
                uint32_t n = 44u;
                acc += 100u;
                if (acc >= 1000u) {
                    acc -= 1000u;
                    n = 45u;
                }
                host_packet(n, amp);
                next_pkt += 0.001;
            }
        }
        for (k = 0; k < HALF_FRAMES; k++) {
            int32_t l, r;
            uint32_t rd = dub_ua_r, fr = dub_rd.frac, back;
            int go = dub_rd.go;
            dub_usb_audio_read_stereo(&l, &r);
            if (go && dub_rd.go && halves > 400u) {    /* where the reader was: the host frame it played */
                double pos, ideal, g = dub_rd.g;
                back = dub_ua_w - rd;                   /* frames between that read and the newest */
                pos = (double)host_frame - (double)back + fr / 65536.0;
                ideal = amp * sin(2.0 * M_PI * 440.0 * pos / 44100.0) * g / 32768.0;
                if (fabs(l - ideal) > maxerr)
                    maxerr = fabs(l - ideal);
            }
        }
        {
            uint32_t fill = dub_ua_w - dub_ua_r;
            if (halves > 400u && dub_rd.go) {          /* (settled) */
                if (fill < *fill_lo)
                    *fill_lo = fill;
                if (fill > *fill_hi)
                    *fill_hi = fill;
                fill_sum += fill;
                fill_n++;
            }
        }
        halves++;
        next_half += half;
    }
    fill_mean = fill_n ? fill_sum / fill_n : 0;
    return maxerr;
}

static void ring(void)
{
    uint32_t lo, hi, u0;
    double err, ratio;
    dub_ctl_defaults();
    dub_ctl.usb_in_q15 = 16384;                         /* the fader at half */
    usb.config = 0;                                     /* (dub_uac_stream: no SIE) */
    dub_uac_stream(1);
    err = run(44117.6, 30.0, 16000.0, &lo, &hi);
    ratio = dub_rd.step / 65536.0;
    printf("  FM-1 at 44117.6 Hz: fill %u..%u mean %.1f (target %u), step %.5f (ideal %.5f), max error %.1f\n", lo,
           hi, fill_mean, DUB_UA_TARGET, ratio, 44100.0 / 44117.6, err);
    check("one start, no underrun, no overrun", dub_rd.starts == 1u && !dub_rd.underruns && !dub_uac.overruns);
    check("fill centres on the target (mean +-8 frames)", fabs(fill_mean - DUB_UA_TARGET) <= 8.0);
    check("fill within the target +- (a render + a packet)",
          lo + HALF_FRAMES + DUB_UA_MAXF >= DUB_UA_TARGET && hi <= DUB_UA_TARGET + HALF_FRAMES + DUB_UA_MAXF);
    check("step settles on the clock ratio (+-0.05 %)", fabs(ratio - 44100.0 / 44117.6) < 0.0005);
    check("plays the sine back (error < 0.1 % of full scale)", err < 32.8);
    check("the fader reached 0.5", abs(dub_rd.g - 16384) < 200);
    check("live", dub_tel.usb_in_live);

    dub_uac_stream(1);                                  /* a new stream, the FM-1 slower than the host */
    u0 = dub_rd.starts;
    err = run(44080.0, 30.0, 16000.0, &lo, &hi);
    ratio = dub_rd.step / 65536.0;
    printf("  FM-1 at 44080 Hz: fill %u..%u mean %.1f, step %.5f (ideal %.5f), max error %.1f\n", lo, hi, fill_mean,
           ratio, 44100.0 / 44080.0, err);
    check("slower FM-1: one new start, no underrun", dub_rd.starts == u0 + 1u && !dub_rd.underruns &&
                                                    !dub_uac.overruns);
    check("slower FM-1: step settles (+-0.05 %)", fabs(ratio - 44100.0 / 44080.0) < 0.0005);
    check("slower FM-1: fill centres on the target (mean +-8 frames)", fabs(fill_mean - DUB_UA_TARGET) <= 8.0);
    check("slower FM-1: fill within the target +- (a render + a packet)",
          lo + HALF_FRAMES + DUB_UA_MAXF >= DUB_UA_TARGET && hi <= DUB_UA_TARGET + HALF_FRAMES + DUB_UA_MAXF);
    check("slower FM-1: plays the sine back", err < 32.8);

    {   /* the host stops: the ring runs dry, the output fades to 0, nothing more is read */
        uint32_t k;
        int32_t l = 0, r = 0, peak_end = 0;
        for (k = 0; k < 8000u; k++) {
            dub_usb_audio_read_stereo(&l, &r);
            if (k > 7000u && abs(l) > peak_end)
                peak_end = abs(l);
        }
        check("host stops: one underrun, faded to 0", dub_rd.underruns == 1u && peak_end == 0 && !dub_rd.go);
        check("host stops: not live", !dub_tel.usb_in_live);
    }
    {   /* alt 0: nothing plays even if packets came */
        int32_t l, r;
        uint32_t k;
        dub_uac_stream(0);
        for (k = 0; k < 10u; k++)
            host_packet(45, 16000.0);
        for (k = 0; k < 2000u; k++)
            dub_usb_audio_read_stereo(&l, &r);
        check("alt 0: silent", l == 0 && r == 0 && !dub_rd.go);
    }
}

int main(void)
{
    srand(1);
    descriptors();
    ring();                                             /* (the mixer intercept with the USB audio: dub_fx_test.c) */
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
