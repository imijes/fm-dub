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
 *   pictures    DIR/tower-*.ppm (idle, playing, everything on) for review, and the audit set as PNG (and PPM):
 *               shot-1-idle, shot-2-overdrive (drive x8, every band near full scale), shot-3-limiter (-10 dB of
 *               gain reduction), shot-4-siren-strobe (the gate open, the LFO at its bright peak), shot-5-wow-spiral
 *               (the echo loud, WOW & FLUTTER shaking the ring), shot-6-locked (opened from the Mix grid: the LOCK
 *               badge); no lamp row on the tower (Phase 5: the punch slots are not drawn there, a slot switching
 *               sends nothing)
 *   fx layer    Phase 5: pads 5..9 dark / armed (blinking) / on (THROW FREEZE SIREN red, DRIFT ROLL gold); the
 *               LATCH (FX + OCT-): THROW / FREEZE / ROLL stay on after the key, the same pad again lets go, a slot
 *               held over it returns to it; the SIREN SOLO (FX + OCT+): pad 9 kills the four bands with the siren
 *               open, pad 9 again ends it
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
/* the screen as a PNG (8-bit RGB, deflate stored blocks: no zlib), for Finder / Preview */
static uint32_t crc_tab[256];
static uint32_t png_crc(uint32_t c, const uint8_t *b, uint32_t n)
{
    uint32_t i, k;
    if (!crc_tab[1])
        for (i = 0; i < 256u; i++) {
            uint32_t v = i;
            for (k = 0; k < 8u; k++)
                v = v & 1u ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    for (i = 0; i < n; i++)
        c = crc_tab[(c ^ b[i]) & 255u] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    uint32_t c;
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n)
        fwrite(d, 1, n, f);
    c = png_crc(0xFFFFFFFFu, h + 4, 4);
    c = png_crc(c, d, n) ^ 0xFFFFFFFFu;
    be32(h, c);
    fwrite(h, 1, 4, f);
}
static void png(const char *name)
{
    static uint8_t raw[240 * (1 + 240 * 3)], z[240 * (1 + 240 * 3) + 1024];
    char path[512];
    FILE *f;
    uint32_t i, x, y, n = sizeof raw, zn = 0, a = 1, b = 0;
    uint8_t ihdr[13] = {0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
    if (!outdir)
        return;
    for (y = 0; y < 240u; y++) {
        uint8_t *r = raw + y * (1u + 240u * 3u);
        r[0] = 0;                                       /* (filter: none) */
        for (x = 0; x < 240u; x++) {
            uint16_t p = swap16(screen[y * 240 + x]);
            r[1 + 3 * x] = (uint8_t)((p >> 11) * 255 / 31);
            r[2 + 3 * x] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            r[3 + 3 * x] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    z[zn++] = 0x78, z[zn++] = 0x01;                     /* zlib, stored blocks */
    for (i = 0; i < n; i += 65535u) {
        uint32_t len = n - i < 65535u ? n - i : 65535u;
        z[zn++] = (uint8_t)(i + len >= n);
        z[zn++] = (uint8_t)len, z[zn++] = (uint8_t)(len >> 8), z[zn++] = (uint8_t)~len, z[zn++] = (uint8_t)(~len >> 8);
        memcpy(z + zn, raw + i, len);
        zn += len;
    }
    for (i = 0; i < n; i++)
        a = (a + raw[i]) % 65521u, b = (b + a) % 65521u;
    be32(z + zn, b << 16 | a);
    zn += 4;
    snprintf(path, sizeof path, "%s/%s.png", outdir, name);
    f = fopen(path, "wb");
    assert(f);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    printf("  snapshot %s\n", path);
}
static void shot(const char *name) { ppm(name); png(name); }

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
    song.g[G_DFDBK] = 125;                         /* (DLY FDBK: the tape echo's) */
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
    /* the audit set: five states, each drawn whole, then a few frames of animation */
    {
        uint32_t f;
        dub_ctl_defaults();
        dub_live.punch = DUB_PX_NONE;
        dub_live.kill = 0;
        memset(&dub_tel, 0, sizeof dub_tel);
        dub_tel.lim_gr_q15 = 32767;
        dub_tel.punch = 0xFFu;
        frame(1);
        {
            int bare = 1;
            uint32_t x, y;
            for (y = 184; y < 220u; y++)
                for (x = 32; x < 208u; x++)
                    bare &= px(x, y) == DUB_UI_BG;
            check("audit: idle: no lamp row on the tower (rows 184..219 bare: the slots are the FX layer's)", bare);
        }
        shot("shot-1-idle");
        dub_ctl.drive_q12 = DUB_DRIVE_MAX;              /* overdrive: drive x8, every band near full scale */
        dub_ctl.tubby_on = 1;
        dub_ctl.tubby_step = 4;
        for (f = 0; f < DUB_NBAND; f++)
            dub_tel.band_pk[f] = (uint16_t)(31000 - f * 900);
        dub_tel.echo_env = 9000;
        frame(1);
        for (f = 0; f < 6u; f++)
            fm1_ms += 40u, frame(0);
        check("audit: overdrive: drive shown x8.0, the cones at full swing", px(32 + 2, 106 + 8) != DUB_UI_BG);
        shot("shot-2-overdrive");
        dub_tel.lim_gr_q15 = 10362;                     /* the limiter clamping: -10 dB */
        fm1_ms += 40u, frame(0);
        check("audit: limiter: the bar short and red", px(DUB_UI_LIM_X + 2, DUB_UI_LIM_Y + 4) == DUB_UI_RED &&
              px(DUB_UI_LIM_X + DUB_UI_LIM_W - 20, DUB_UI_LIM_Y + 4) == DUB_UI_PLATE);
        shot("shot-3-limiter");
        dub_tel.lim_gr_q15 = 32767;
        dub_ctl.siren_gate = 1;                         /* the siren: the gate open, the LFO at its brightest */
        dub_tel.siren_ph = 32767;
        dub_tel.siren_pan = 20000;
        fm1_ms += 40u, frame(0);
        check("audit: siren: the strobe border lit", px(DUB_UI_SCOPE_X + 1, DUB_UI_SCOPE_Y + 1) != DUB_UI_PLATE);
        shot("shot-4-siren-strobe");
        dub_ctl.siren_gate = 0;
        dub_tel.echo_env = 30000;                       /* the spiral: a loud echo, WOW & FLUTTER shaking it */
        dub_live.punch = DUB_PX_WOW;
        song.g[G_DFDBK] = 125;                         /* (DLY FDBK: the tape echo's) */
        for (f = 0; f < 9u; f++)
            fm1_ms += 40u, frame(0);
        shot("shot-5-wow-spiral");
        dub_live.punch = DUB_PX_NONE;
        fm1_ms += 40u, frame(0);
        {   /* a punch slot switching sends nothing to the tower (no lamps there) */
            static const int8_t SLOT[4] = {DUB_PX_ECHO_THROW, DUB_PX_ECHO_FREEZE, DUB_PX_STUTTER, DUB_PX_NONE};
            uint32_t k, n = 0;
            dub_tel.echo_env = 0;                       /* (the scope still) */
            memset((void *)dub_tel.band_pk, 0, sizeof dub_tel.band_pk);
            song.g[G_DFDBK] = 60;
            fm1_ms += 40u, frame(0);
            fm1_ms += 40u, frame(0);
            for (k = 0; k < 4u; k++) {
                dub_live.punch = SLOT[k];
                fm1_ms += 40u;
                n += frame(0);
            }
            printf("  THROW, FREEZE, ROLL, none on the tower: %u bytes sent\n", n);
            check("audit: the slots switching send nothing to the tower (no lamp row)", n == 0u);
        }
        {   /* opened from the Mix grid: latched, the LOCK badge top right; HOME's close lets it go */
            uint32_t x, y, gold = 0, title = 1;
            dub_tower_latch(dub_ui.page, dub_ui.home);
            frame(1);
            for (y = 3; y < 18u; y++)
                for (x = 196; x < 236u; x++)
                    gold += px(x, y) == DUB_UI_GOLD;
            for (y = 0; y < 20u; y++)                   /* the title's letters stay clear of the badge */
                for (x = 32; x < 236u - (text_w(&FONT_S, "LOCK") + 8u); x++)
                    title &= px(x, y) != DUB_UI_GOLD;
            check("audit: latched: the LOCK badge, gold, top right", dub_ui.lock && gold > 200u);
            check("audit: latched: the title clear of the badge", title);
            shot("shot-6-locked");
            dub_tower_close();
            check("audit: HOME's close: unlatched", !dub_ui.on && !dub_ui.lock);
            dub_tower_open(dub_ui.page, dub_ui.home);
            frame(1);
            check("audit: opened from the menu: no badge", !dub_ui.lock && px(230, 8) == DUB_UI_BG);
        }
    }
    /* the FX layer (Phase 5): the pads' three states, the LATCH, the SIREN SOLO (dub_midi.c, as seq.c calls it) */
    {
        uint16_t bg, fg;
        uint32_t k;
        int ok = 1;
        dub_ctl_defaults();
        memset(&dub_live, 0, sizeof dub_live);
        dub_live.punch = dub_live.latched = DUB_PX_NONE;
        memset(&dub_pad, 0, sizeof dub_pad);
        for (k = 4; k < 9u; k++)
            ok &= dub_fx_tile(k, 1, &bg, &fg) && bg == DUB_UI_PLATE && fg == DUB_UI_GRAY;
        check("fx: pads 5..9 dark, gray text", ok);
        check("fx: the other pads are not these tiles", !dub_fx_tile(0, 1, &bg, &fg) && !dub_fx_tile(9, 1, &bg, &fg));
        ok = 1;
        for (k = 4; k < 9u; k++) {
            dub_pad_down(k, 1u << k);
            ok &= dub_fx_tile(k, 0, &bg, &fg) && fg == C_BLACK && bg == (k == 6u || k == 7u ? DUB_UI_GOLD : DUB_UI_RED);
            dub_pad_up(1u << k);
            ok &= dub_live.punch == DUB_PX_NONE;
        }
        check("fx: held: a solid block (THROW FREEZE SIREN red, DRIFT ROLL gold); let go: off", ok);
        dub_ctl.latch_arm = (uint8_t)(dub_live.latch_ack ^ 1u);   /* FX + OCT- */
        ok = 1;
        for (k = 4; k < 9u; k++) {
            int lit = dub_fx_tile(k, 1, &bg, &fg) && fg == C_WHITE, dim = dub_fx_tile(k, 0, &bg, &fg) && fg == DUB_UI_GRAY;
            ok &= (k == 4u || k == 5u || k == 7u) ? lit && dim : !lit;
        }
        check("fx: latch armed: THROW FREEZE ROLL blink white, DRIFT and SIREN do not", ok);
        dub_pad_down(5, 1u << 5);                       /* FREEZE, latched */
        dub_pad_up(1u << 5);
        check("fx: latch: FREEZE stays on after its key, the arm is taken",
              dub_live.punch == DUB_PX_ECHO_FREEZE && dub_live.latched == DUB_PX_ECHO_FREEZE &&
              dub_ctl.latch_arm == dub_live.latch_ack && dub_fx_tile(5, 1, &bg, &fg) && bg == DUB_UI_RED);
        dub_pad_down(4, 1u << 4);                       /* THROW held over it */
        check("fx: latch: THROW held over it plays", dub_live.punch == DUB_PX_ECHO_THROW);
        dub_pad_up(1u << 4);
        check("fx: latch: THROW let go, FREEZE back", dub_live.punch == DUB_PX_ECHO_FREEZE);
        dub_pad_down(5, 1u << 5);                       /* FREEZE again: let go */
        dub_pad_up(1u << 5);
        check("fx: latch: the same pad again lets it go", dub_live.punch == DUB_PX_NONE && dub_live.latched == DUB_PX_NONE);
        dub_pad_down(4, 1u << 4);                       /* not armed: momentary again */
        dub_pad_up(1u << 4);
        check("fx: latch: one arm, one latch (THROW after it: momentary)", dub_live.punch == DUB_PX_NONE);
        dub_ctl.latch_arm = (uint8_t)(dub_live.latch_ack ^ 1u);
        dub_pad_down(6, 1u << 6);                       /* DRIFT does not take the latch */
        dub_pad_up(1u << 6);
        check("fx: latch: DRIFT stays momentary, the arm waits", dub_live.punch == DUB_PX_NONE &&
              dub_ctl.latch_arm != dub_live.latch_ack);
        dub_ctl.latch_arm = dub_live.latch_ack;         /* (FX + OCT- again: disarmed) */
        dub_ctl.solo_arm = (uint8_t)(dub_live.solo_ack ^ 1u);   /* FX + OCT+ */
        check("fx: siren solo armed: SIREN blinks red", dub_fx_tile(8, 1, &bg, &fg) && fg == DUB_UI_RED &&
              dub_fx_tile(8, 0, &bg, &fg) && fg == DUB_UI_GRAY);
        dub_pad_down(8, 1u << 8);
        dub_pad_up(1u << 8);
        check("fx: siren solo: pad 9 kills the four bands, the siren open, its tile solid red",
              dub_live.siren_solo && dub_kill_now() == DUB_KILL_ALL && dub_ctl.solo_arm == dub_live.solo_ack &&
              dub_ui_siren_open() && dub_fx_tile(8, 1, &bg, &fg) && bg == DUB_UI_RED);
        dub_pad_down(8, 1u << 8);
        dub_pad_up(1u << 8);
        check("fx: siren solo: pad 9 again ends it, the bands back", !dub_live.siren_solo && dub_kill_now() == 0u &&
              dub_live.punch == DUB_PX_NONE);
        dub_pad_down(0, 1u);                            /* a pad kill held through a solo stays killed after it */
        dub_ctl.solo_arm = (uint8_t)(dub_live.solo_ack ^ 1u);
        dub_pad_down(8, 1u << 8), dub_pad_up(1u << 8);
        dub_pad_down(8, 1u << 8), dub_pad_up(1u << 8);
        check("fx: siren solo over a held kill: that band stays killed", dub_kill_now() == DUB_KILL_SUB);
        dub_pad_up(1u);
        check("fx: and opens with its key", dub_kill_now() == 0u);
    }
    /* the knobs */
    dub_ctl_defaults();
    for (i = 0; i < 40u; i++)
        dub_tower_knob(0, 1), dub_tower_knob(1, 3), dub_tower_knob(2, 5), dub_tower_knob(3, 4);
    check("knobs: 1 big knob (to 10, in), 2 drive (to x8), 3 feedback (to 1.25), 4 siren (to C8): all held in range",
          dub_ctl.tubby_step == 10 && dub_ctl.tubby_on && dub_ctl.drive_q12 == DUB_DRIVE_MAX &&
          song.g[G_DFDBK] == 125 && dub_ctl.siren_note_q8 == 108 << 8);
    for (i = 0; i < 400u; i++)
        dub_tower_knob(0, -1), dub_tower_knob(1, -3), dub_tower_knob(2, -5), dub_tower_knob(3, -4);
    check("knobs: and down to FLAT, x1, 0, C1", dub_ctl.tubby_step == 0 && dub_ctl.drive_q12 == DUB_Q12_ONE &&
          song.g[G_DFDBK] == 0 && dub_ctl.siren_note_q8 == 24 << 8);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
