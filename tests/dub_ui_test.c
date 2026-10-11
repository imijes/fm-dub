/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's PREAMP CONTROL TOWER (firmware/src/dub_preamp/dub_ui_dashboard.c) on a
 * framebuffer that counts what is sent to the panel:
 *   drawing     the whole page on a forced redraw; everything inside the 240 x 240 frame; the fixed background
 *   dirty       nothing changed: nothing sent; a change of one thing: only its rectangle; the moving parts at
 *               most 30 times a second; the bytes per frame with music, echo, siren, kills (reported, and kept
 *               far under a full screen)
 *   parts       the big knob's 11 pointer positions all differ; a killed band's tile turns red with an X; the
 *               limiter's bar shortens with the gain reduction; the strobe follows the siren's LFO while its gate
 *               is open, and is plain when shut; WOW shakes the ring; the knobs set dub_ctl within its ranges
 *   pictures    DIR/tower-*.ppm (idle, playing, everything on) for review
 * usage: dub_ui_test [DIR] */
#include <stdint.h>
#include <assert.h>
#define ENABLE_DUB_PREAMP
#define main hostsim_main                               /* SLOOP on the host: as regress.c */
#include "hostsim.c"
#undef main

static uint16_t screen[240 * 240];
static uint32_t sent;                                   /* bytes to the panel */
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    assert(x + w <= 240 && y + h <= 240);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            screen[(y + j) * 240 + x + i] = p[j * w + i];
    sent += w * h * 2u;
}
#include "../firmware/src/gfx.c"
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    assert(x + w <= 240 && y + h <= 240);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            screen[(y + j) * 240 + x + i] = swap16(c);
    sent += w * h * 2u;
}
#include "../firmware/src/dub_preamp/dub_ui_dashboard.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static uint16_t px(uint32_t x, uint32_t y) { return swap16(screen[y * 240 + x]); }
static const char *outdir;
static void ppm(const char *name)
{
    char path[512];
    FILE *f;
    uint32_t i;
    if (!outdir)
        return;
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t p = swap16(screen[i]);
        uint8_t rgb[3] = {(uint8_t)((p >> 11) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static uint32_t frame(int force)                        /* one main-loop frame, 16 ms on */
{
    sent = 0;
    fm1_ms += 16u;
    dub_tower_draw(force);
    assert(sent == dub_ui.bytes);                       /* (the page's own count agrees with the panel's) */
    return sent;
}
static void music(uint32_t t)                           /* the audio's telemetry, moving (the limiter: as set) */
{
    uint32_t b;
    for (b = 0; b < DUB_NBAND; b++)
        dub_tel.band_pk[b] = (uint16_t)(8000 + 7000 * sin(t * 0.37 + b * 1.7));
    dub_tel.echo_env = (uint16_t)(6000 + 5000 * sin(t * 0.11));
}

int main(int argc, char **argv)
{
    uint32_t i, n, max = 0, tot = 0, k;
    outdir = argc > 1 ? argv[1] : NULL;
    dub_ctl_defaults();
    dub_live.punch = DUB_PX_NONE;
    memset(&dub_tel, 0, sizeof dub_tel);
    dub_tel.lim_gr_q15 = 32767;
    dub_tel.punch = 0xFFu;
    song.g[G_BPM] = 120;
    /* the whole page */
    n = frame(1);
    printf("  forced: %u bytes (a full screen is %u)\n", n, 240u * 240u * 2u);
    check("forced redraw: the whole page", n >= 240u * 240u * 2u);
    check("the background is #121212 (0x10A2), fixed", px(120, 236) == 0x10A2u && px(2, 2) == 0x10A2u);
    ppm("tower-idle");
    /* nothing moves: nothing sent */
    for (i = 0, n = 0; i < 20u; i++)
        n += frame(0);
    check("nothing changed: nothing sent (20 frames, 0 bytes)", n == 0);
    /* one thing: the big knob */
    dub_ctl.tubby_on = 1;
    dub_ctl.tubby_step = 3;
    n = frame(0);
    printf("  the big knob turned: %u bytes\n", n);
    check("the big knob turned: only the dial and its value (16x16 + 120x16)", n == (16u * 16u + 120u * 16u) * 2u);
    {   /* its 11 positions: each picture of the dial differs */
        static uint16_t pic[DUB_TUBBY_NSTEP][256];
        int distinct = 1;
        for (k = 0; k < DUB_TUBBY_NSTEP; k++) {
            dub_ctl.tubby_step = (uint8_t)k;
            frame(0);
            for (i = 0; i < 256u; i++)
                pic[k][i] = screen[(DUB_UI_KNOB_Y + i / 16u) * 240 + DUB_UI_KNOB_X + i % 16u];
        }
        for (k = 0; k < DUB_TUBBY_NSTEP; k++)
            for (i = k + 1u; i < DUB_TUBBY_NSTEP; i++)
                distinct &= memcmp(pic[k], pic[i], sizeof pic[k]) != 0;
        check("the big knob: 11 positions, 11 different pointers", distinct);
    }
    /* a kill: its tile red with an X, only the tiles (and the drivers' tint) sent */
    dub_ctl.kill = DUB_KILL_MID;
    frame(0);
    check("MID killed: its tile solid red", px(DUB_UI_KILL_X + 1 + 2, DUB_UI_KILL_Y + 20 + 2) == DUB_UI_RED);
    check("... the others not", px(DUB_UI_KILL_X + 15 + 2, DUB_UI_KILL_Y + 20 + 2) == DUB_UI_BG);
    dub_live.kill = DUB_KILL_SUB;
    frame(0);
    check("SUB killed by MIDI (dub_live): its tile red too", px(DUB_UI_KILL_X + 1 + 2, DUB_UI_KILL_Y + 2 + 2) == DUB_UI_RED);
    dub_ctl.kill = 0;
    dub_live.kill = 0;
    frame(0);
    /* the limiter: -6 dB pulls the bar to half */
    {
        uint32_t full = 0, half = 0, x;
        for (x = 0; x < DUB_UI_LIM_W; x++)
            full += px(DUB_UI_LIM_X + x, DUB_UI_LIM_Y + 4) != DUB_UI_PLATE;
        dub_tel.lim_gr_q15 = 16384;                     /* -6.02 dB */
        fm1_ms += 40u;
        frame(0);
        for (x = 0; x < DUB_UI_LIM_W; x++)
            half += px(DUB_UI_LIM_X + x, DUB_UI_LIM_Y + 4) != DUB_UI_PLATE;
        printf("  limiter: lit %u px at 0 dB, %u at -6 dB\n", full, half);
        check("the limiter's bar: -6 dB of reduction halves it (snaps left)", half * 2u > full - 8u && half * 2u < full + 8u);
        check("... and turns red (past 3 dB)", px(DUB_UI_LIM_X + 2, DUB_UI_LIM_Y + 4) == DUB_UI_RED);
        dub_tel.lim_gr_q15 = 32767;
    }
    /* the siren's strobe: plain while shut, following the LFO while open */
    {
        uint16_t shut = px(DUB_UI_SCOPE_X + 1, DUB_UI_SCOPE_Y + 1), a, b;
        dub_ctl.siren_gate = 1;
        dub_tel.siren_ph = 0;
        fm1_ms += 40u;
        frame(0);
        a = px(DUB_UI_SCOPE_X + 1, DUB_UI_SCOPE_Y + 1);
        dub_tel.siren_ph = 32767;
        fm1_ms += 40u;
        frame(0);
        b = px(DUB_UI_SCOPE_X + 1, DUB_UI_SCOPE_Y + 1);
        check("strobe: plain with the siren shut", shut == DUB_UI_PLATE);
        check("strobe: dark at the LFO's start, bright at its middle", a == DUB_UI_STROBE_LO && b != a && (b >> 11) > (a >> 11));
        dub_ctl.siren_gate = 0;
        fm1_ms += 40u;
        frame(0);
        check("strobe: plain again when the gate shuts", px(DUB_UI_SCOPE_X + 1, DUB_UI_SCOPE_Y + 1) == DUB_UI_PLATE);
    }
    /* at most 30 frames a second for the moving parts */
    music(0);
    fm1_ms += 40u;
    frame(0);
    music(1);
    n = frame(0);                                       /* 16 ms later: too soon */
    check("the moving parts wait 33 ms (a frame 16 ms after one: nothing)", n == 0);
    /* playing: the bytes per frame */
    for (i = 0; i < 600u; i++) {
        music(i);
        n = frame(0);
        tot += n;
        if (n > max)
            max = n;
        if (i == 300u)
            ppm("tower-playing");
    }
    printf("  playing (bands, echo moving): %u bytes a frame on average, %u at most (%.1f %% / %.1f %% of a "
           "full screen), %.0f kB/s\n", tot / 600u, max, tot / 600.0 / 1152.0, max / 1152.0, tot / 600.0 * 62.5 / 1000);
    check("playing: well under a full screen a frame (average < 15 %, worst < 25 %)",
          tot / 600u < 240u * 240u * 2u * 15u / 100u && max < 240u * 240u * 2u / 4u);
    /* everything on: kills, siren, WOW, a punch, the flags */
    dub_ctl.kill = DUB_KILL_BASS | DUB_KILL_TREBLE;
    dub_ctl.siren_gate = 1;
    dub_live.punch = DUB_PX_WOW;
    dub_ctl.echo_to_rev = 1;
    dub_ctl.siren_sync = 1;
    dub_ctl.sub_arm = 1;
    dub_tel.sub_on = 1;
    dub_tel.usb_in_live = 1;
    dub_ctl.echo_fb_q12 = DUB_FB_MAX;
    dub_tel.lim_gr_q15 = 20675;
    tot = max = 0;
    for (i = 0; i < 300u; i++) {
        music(i);
        dub_tel.siren_ph = (uint16_t)(i * 2400u);
        n = frame(0);
        tot += n;
        if (n > max)
            max = n;
        if (i == 150u)
            ppm("tower-all");
    }
    printf("  everything on: %u bytes a frame on average, %u at most (%.1f %% / %.1f %%)\n", tot / 300u, max,
           tot / 300.0 / 1152.0, max / 1152.0);
    {   /* WOW: the same telemetry twice, the ring differs (it shakes) */
        static uint16_t s1[64 * 64], s2[64 * 64];
        uint32_t x, y;
        music(5);
        dub_tel.siren_ph = 0;
        fm1_ms += 40u;
        frame(0);
        for (y = 0; y < 64u; y++)
            for (x = 0; x < 64u; x++)
                s1[y * 64 + x] = screen[(90 + y) * 240 + 88 + x];
        dub_ui.rot -= 8u + (uint32_t)(dub_ui_lvl(dub_tel.echo_env) >> 4);   /* (the same turn as before) */
        fm1_ms += 40u;
        frame(0);
        for (y = 0; y < 64u; y++)
            for (x = 0; x < 64u; x++)
                s2[y * 64 + x] = screen[(90 + y) * 240 + 88 + x];
        check("WOW & FLUTTER: the ring shakes (the same picture differs frame to frame)", memcmp(s1, s2, sizeof s1) != 0);
    }
    /* the knobs */
    dub_ctl_defaults();
    for (i = 0; i < 40u; i++)
        dub_tower_knob(0, 1), dub_tower_knob(1, 3), dub_tower_knob(2, 5), dub_tower_knob(3, 4);
    check("knobs: 1 big knob (to 10, in), 2 drive (to x8), 3 feedback (to 1.25), 4 siren (to C8): all held in range",
          dub_ctl.tubby_step == 10 && dub_ctl.tubby_on && dub_ctl.drive_q12 == DUB_DRIVE_MAX &&
          dub_ctl.echo_fb_q12 == DUB_FB_MAX && dub_ctl.siren_note_q8 == 108 << 8);
    for (i = 0; i < 400u; i++)
        dub_tower_knob(0, -1), dub_tower_knob(1, -3), dub_tower_knob(2, -5), dub_tower_knob(3, -4);
    check("knobs: and down to FLAT, x1, 0, C1", dub_ctl.tubby_step == 0 && dub_ctl.drive_q12 == DUB_Q12_ONE &&
          dub_ctl.echo_fb_q12 == 0 && dub_ctl.siren_note_q8 == 24 << 8);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
