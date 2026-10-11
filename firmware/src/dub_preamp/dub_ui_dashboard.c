/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 4.1: the PREAMP CONTROL TOWER, a rack-mounted dub preamp between two speaker stacks.
 *
 *   x:  4 .. 27        32 .. 208                                                     212 .. 235
 *   y   +------+  PREAMP CONTROL TOWER (title, 2..17)                                   +------+
 *   24  | TWT  |  (36,28) big knob 16x16  HPF 150                (180,28) kill tiles  | TWT  |
 *       | MID  |  rows 48..60: the limiter's gain (0 .. -12 dB), snapping left       | MID  |
 *       |      |  DRIVE        +--- (84,86) 72x72: the strobe border ---+  SIREN      |      |
 *       | BASS |  X2.0         |  (88,90) 64x64 scope: the tape vortex   |  D5        | BASS |
 *       |      |  FDBK         |  ring, SUB / TREBLE bars                |  ECHO      |      |
 *       | SUB  |  0.60         +-----------------------------------------+  1/8D      | SUB  |
 *       |      |  flags (166): E>R BYP CLK ARM; the punch slot (186, large); MIDI CH, SUB USB |      |
 *   235 +------+                                                                      +------+
 *
 * Colours are fixed (a steel faceplate: the menu's COLOR does not repaint it); the background is #121212
 * (RGB565 0x10A2). The speakers' cones (four drivers a side: tweeter, mid, bass, sub) breathe with
 * dub_tel.band_pk; the big knob's pointer takes one of 11 positions (dub_ctl.tubby_step, SINE for the angle);
 * a killed band's tile turns solid red with an X (dub_ctl.kill | dub_live.kill); the limiter's bar is the gain
 * (dub_tel.lim_gr_q15) on a dB scale; the scope draws the echo as a rotating spiral ring (dub_tel.echo_env),
 * shaken by WOW & FLUTTER (the UI's own LFSR); the border flashes with the siren's LFO (dub_tel.siren_ph)
 * while its gate is open.
 *
 * The screen is written only where something changed: every element is its own rectangle, drawn into the
 * canvas and sent when its picture's signature differs from the last one sent (the static faceplate: once, on
 * a forced redraw). The moving parts are drawn at most 30 times a second. tests/dub_ui_test.c counts the
 * bytes sent per frame.
 *
 * Main loop only (ui_draw.c, ui_input.c): it reads dub_ctl, dub_live and dub_tel, writes dub_ctl (the knobs).
 * Opened from the menu (DUB > CONTROL TOWER), closed by HOME or by any other page (ui_input.c, ui_draw.c); a
 * layer held, the REC screen or the menu show over it as they do over any page, and it comes back whole. KNOB 1..4
 * set the big knob, the drive, the echo feedback, the siren's pitch; the other controls work as on the TRACKS
 * screen. Included by felucca.c before ui_draw.c. */
#ifdef ENABLE_DUB_PREAMP

/* ------------------------------------------------------------- the faceplate --- */
#define DUB_UI_BG 0x10A2u                      /* #121212, matte steel charcoal */
#define DUB_UI_PLATE RGB(44, 46, 48)           /* the rack's raised panels */
#define DUB_UI_EDGE RGB(78, 80, 84)            /* their bevels, the rules */
#define DUB_UI_SILVER RGB(196, 198, 200)       /* engraved labels */
#define DUB_UI_GRAY RGB(120, 122, 126)
#define DUB_UI_GOLD RGB(255, 196, 24)          /* values */
#define DUB_UI_RED RGB(232, 40, 32)
#define DUB_UI_GREEN RGB(40, 200, 80)
#define DUB_UI_CABINET RGB(26, 24, 22)         /* the speaker boxes */
#define DUB_UI_CONE RGB(70, 70, 72)
#define DUB_UI_STROBE_LO RGB(60, 20, 0)
#define DUB_UI_STROBE_HI RGB(255, 120, 0)

#define DUB_UI_SPK_W 24                        /* a speaker stack, x 4 and 212 */
#define DUB_UI_SPK_Y 24
#define DUB_UI_SPK_H 212
static const uint8_t DUB_UI_DRV_Y[DUB_NBAND] = {190, 132, 76, 38};   /* the drivers' 24x24 cells' tops, SUB .. TREBLE
                                                                      * (the sub and bass box under the rule at 112) */
static const uint8_t DUB_UI_DRV_R[DUB_NBAND] = {11, 10, 7, 5};       /* their cone radii */
#define DUB_UI_KNOB_X 36
#define DUB_UI_KNOB_Y 28
#define DUB_UI_KILL_X 180
#define DUB_UI_KILL_Y 28
#define DUB_UI_KILL_W 30
#define DUB_UI_KILL_H 40
#define DUB_UI_LIM_X 36
#define DUB_UI_LIM_Y 48
#define DUB_UI_LIM_W 136
#define DUB_UI_LIM_H 13                        /* rows 48 .. 60 */
#define DUB_UI_SCOPE_X 84                      /* the strobe border: 72x72 around the 64x64 scope at (88, 90) */
#define DUB_UI_SCOPE_Y 86
#define DUB_UI_SCOPE_S 72
#define DUB_UI_FRAME_MS 33u                    /* the moving parts: at most 30 a second */

static const char *const DUB_UI_HPF[DUB_TUBBY_NSTEP] = {"FLAT", "70", "100", "150", "250", "500", "1K", "2K",
                                                        "3K", "5K", "7.5K"};
static const char *const DUB_UI_PUNCH[DUB_NSLOT] = {"KILL SUB", "KILL BASS", "KILL MID", "KILL TREB", "THROW",
                                                    "FREEZE", "WOW", "STUTTER", "SIREN", "CRASH", "SPINBACK",
                                                    "TAPE STOP", "FILTER", "BLACK ARK", "CHOP", "DUCK"};
static const char *const DUB_UI_DLY[NDIV_DLY] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T", "1/8D", "1/16D"};
static const char *const DUB_UI_NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* the gain (Q15) at 0, -0.5, .. -12 dB: the limiter's scale (25 marks) */
static const uint16_t DUB_UI_DB[25] = {32767, 30934, 29204, 27571, 26029, 24573, 23198, 21900, 20675, 19519,
                                       18427, 17396, 16423, 15504, 14637, 13818, 13045, 12315, 11627, 10976,
                                       10362, 9783, 9235, 8719, 8231};

static struct {
    uint8_t on;                                /* opened (the menu), closed by HOME */
    uint8_t shown;                             /* drawn last frame (ui_draw.c: the page behind is redrawn after) */
    uint8_t page, home;                        /* ui.page / ui.home it was opened on: another page closes it */
    uint32_t last_ms;                          /* the last frame the moving parts were drawn */
    uint32_t sig_drv[DUB_NBAND], sig_knob, sig_kill, sig_lim, sig_scope, sig_txt[9];
    uint32_t lfsr;                             /* WOW's wobble on the ring */
    uint32_t rot;                              /* the vortex's turn (1024 a revolution) */
    uint32_t bytes;                            /* sent to the panel by the last frame (tests) */
} dub_ui = {.lfsr = 0x1D872B41u};

static void dub_ui_send(uint32_t x, uint32_t y)            /* the canvas to the screen, counted */
{
    cv_blit(x, y);
    dub_ui.bytes += cv_w * cv_h * 2u;
}

/* text on the canvas blended onto bg (cv_text blends onto black: a dark fringe on the steel) */
static int32_t dub_ui_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg)
{
    uint16_t ramp[16];
    uint32_t a;
    for (a = 0; a < 16u; a++) {
        int32_t k = (int32_t)(a * 256u / 15u);
        int32_t r = (bg >> 11) + ((((c >> 11) - (bg >> 11)) * k) >> 8);
        int32_t g = ((bg >> 5) & 63) + (((((c >> 5) & 63) - ((bg >> 5) & 63)) * k) >> 8);
        int32_t b = (bg & 31) + ((((c & 31) - (bg & 31)) * k) >> 8);
        ramp[a] = (uint16_t)(r << 11 | g << 5 | b);
    }
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w, bpr;
        const uint8_t *gd;
        w = f->bw[gi];
        bpr = ((w >> f->sh) + 1u) / 2u;
        gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t sx = gx >> f->sh, v = gd[(gy >> f->sh) * bpr + sx / 2u];
                v = (sx & 1u) ? (v & 15u) : (v >> 4);
                if (v)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}

static uint16_t dub_ui_mix(uint16_t a, uint16_t b, int32_t k)   /* a -> b, k 0..256 */
{
    int32_t r = (a >> 11) + ((((b >> 11) - (a >> 11)) * k) >> 8);
    int32_t g = ((a >> 5) & 63) + (((((b >> 5) & 63) - ((a >> 5) & 63)) * k) >> 8);
    int32_t bl = (a & 31) + ((((b & 31) - (a & 31)) * k) >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* a level (Q15 peak) on a 48 dB scale, 0..256 (6 dB a step of 32: log2 with 3 fraction bits) */
static int32_t dub_ui_lvl(uint32_t a)
{
    int32_t lg = 0, v;
    if (a < 16u)
        return 0;
    while ((a >> lg) > 1u)
        lg++;
    v = lg * 8 + (int32_t)(((a << 3) >> lg) & 7u);            /* 120 = 0 dB (32767) */
    v = (v - 56) * 4;
    return v < 0 ? 0 : v > 256 ? 256 : v;
}

static void dub_ui_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)   /* a circle, 64 chords */
{
    uint32_t k;
    int32_t px = cx, py = cy - r;
    for (k = 1; k <= 64u; k++) {
        int32_t x = cx + ((SINE[(k * 16u) & 1023u] * r + 16384) >> 15);
        int32_t y = cy - ((SINE[(k * 16u + 256u) & 1023u] * r + 16384) >> 15);
        cv_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
static void dub_ui_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)   /* a filled circle */
{
    int32_t y;
    for (y = -r; y <= r; y++) {
        int32_t w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r)
            w++;
        cv_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

static int dub_ui_siren_open(void)
{
    return dub_ctl.siren_gate || dub_live.siren_gate || dub_punch_now() == DUB_PX_SIREN;
}

/* ----------------------------------------------------------------- the parts --- */
/* the faceplate: the cabinets, the title, the engraved labels; on a forced redraw only */
static void dub_ui_plate(void)
{
    uint32_t side;
    lcd_fill(0, 0, 240, 240, DUB_UI_BG);
    dub_ui.bytes += 240u * 240u * 2u;
    cv_begin(176, 20, DUB_UI_BG);                               /* the title */
    dub_ui_text((176 - text_w(&FONT_S, "PREAMP CONTROL TOWER")) / 2, 2, &FONT_S, "PREAMP CONTROL TOWER",
                DUB_UI_SILVER, DUB_UI_BG);
    cv_rect(0, 19, 176, 1, DUB_UI_EDGE);
    dub_ui_send(32, 0);
    for (side = 0; side < 2u; side++) {                         /* the cabinets: a box, the bevel, the screws */
        uint32_t x = side ? 212u : 4u;
        lcd_fill(x, DUB_UI_SPK_Y, DUB_UI_SPK_W, DUB_UI_SPK_H, DUB_UI_CABINET);
        lcd_fill(x, DUB_UI_SPK_Y, DUB_UI_SPK_W, 1, DUB_UI_EDGE);
        lcd_fill(x, DUB_UI_SPK_Y + DUB_UI_SPK_H - 1u, DUB_UI_SPK_W, 1, DUB_UI_EDGE);
        lcd_fill(x, DUB_UI_SPK_Y, 1, DUB_UI_SPK_H, DUB_UI_EDGE);
        lcd_fill(x + DUB_UI_SPK_W - 1u, DUB_UI_SPK_Y, 1, DUB_UI_SPK_H, DUB_UI_EDGE);
        lcd_fill(x, 112, DUB_UI_SPK_W, 2, DUB_UI_EDGE);         /* the stack: two boxes, the sub's at the foot */
        dub_ui.bytes += (DUB_UI_SPK_W * DUB_UI_SPK_H + 4u * DUB_UI_SPK_W + 2u * DUB_UI_SPK_H) * 2u;
    }
    cv_begin(48, 72, DUB_UI_BG);                                /* the engraved labels beside the scope */
    dub_ui_text(0, 4, &FONT_S, "DRIVE", DUB_UI_GRAY, DUB_UI_BG);      /* (screen y 90; the values at 106, 142) */
    dub_ui_text(0, 40, &FONT_S, "FDBK", DUB_UI_GRAY, DUB_UI_BG);
    dub_ui_send(32, 86);
    cv_begin(48, 72, DUB_UI_BG);
    dub_ui_text(0, 4, &FONT_S, "SIREN", DUB_UI_GRAY, DUB_UI_BG);
    dub_ui_text(0, 40, &FONT_S, "ECHO", DUB_UI_GRAY, DUB_UI_BG);
    dub_ui_send(160, 86);
    lcd_fill(32, 162, 176, 1, DUB_UI_EDGE);                     /* the rule under the scope */
    dub_ui.bytes += 176u * 2u;
}

/* one driver of both stacks: its cone breathes with the band's level */
static void dub_ui_driver(uint32_t b, int force)
{
    int32_t lv = dub_ui_lvl(dub_tel.band_pk[b]) >> 4, r = DUB_UI_DRV_R[b], c = 12;   /* 0..16 */
    int32_t ex = (lv * (r / 3 + 1)) >> 4;                       /* the excursion, px */
    uint16_t lit = dub_ui_mix(DUB_UI_CONE, (dub_ctl.kill | dub_live.kill) >> b & 1u ? DUB_UI_RED : DUB_UI_GOLD,
                              lv * 16);
    uint32_t sig = (uint32_t)lv * 2u + ((dub_ctl.kill | dub_live.kill) >> b & 1u) + 1u;
    if (!force && sig == dub_ui.sig_drv[b])
        return;
    dub_ui.sig_drv[b] = sig;
    cv_begin(DUB_UI_SPK_W, 24, DUB_UI_CABINET);
    dub_ui_disc(c - 1, c - 1, r, RGB(10, 10, 10));              /* the baffle's hole */
    dub_ui_ring(c - 1, c - 1, r, DUB_UI_EDGE);                  /* the surround */
    dub_ui_ring(c - 1, c - 1, r - 2 + (ex > 1 ? 1 : 0), DUB_UI_CONE);
    if (r > 6)
        dub_ui_ring(c - 1, c - 1, r / 2 + ex, lit);             /* the cone's ridge, pushed out by the level */
    dub_ui_disc(c - 1, c - 1, r / 4 + (ex >> 1), lit);          /* the dust cap */
    dub_ui_send(4, DUB_UI_DRV_Y[b]);                            /* the same driver on both stacks */
    dub_ui_send(212, DUB_UI_DRV_Y[b]);
}

/* the big knob: a 16x16 dial, its pointer at one of 11 positions (7 o'clock .. 5 o'clock); the step beside it */
static void dub_ui_knob(int force)
{
    uint32_t step = dub_ctl.tubby_on ? dub_ctl.tubby_step : 0u, sig;
    int32_t a, k;
    if (step >= DUB_TUBBY_NSTEP)
        step = DUB_TUBBY_NSTEP - 1u;
    sig = step + 1u + ((uint32_t)dub_tel.tubby_step << 8);
    if (!force && sig == dub_ui.sig_knob)
        return;
    dub_ui.sig_knob = sig;
    cv_begin(16, 16, DUB_UI_BG);
    dub_ui_disc(7, 7, 7, DUB_UI_PLATE);
    dub_ui_ring(7, 7, 7, DUB_UI_EDGE);
    a = -427 + (int32_t)step * 85;                              /* -150 .. +150 degrees, 1024 a turn */
    k = (int32_t)((uint32_t)a & 1023u);
    cv_line(7, 7, 7 + ((SINE[k] * 6 + 16384) >> 15), 7 - ((SINE[(k + 256) & 1023] * 6 + 16384) >> 15),
            step ? DUB_UI_GOLD : DUB_UI_SILVER);
    dub_ui_send(DUB_UI_KNOB_X, DUB_UI_KNOB_Y);
    cv_begin(120, 16, DUB_UI_BG);                               /* "HPF 150" */
    k = dub_ui_text(0, 0, &FONT_S, "HPF ", DUB_UI_GRAY, DUB_UI_BG);
    dub_ui_text(k, 0, &FONT_S, DUB_UI_HPF[step], step ? DUB_UI_GOLD : DUB_UI_SILVER, DUB_UI_BG);
    dub_ui_send(DUB_UI_KNOB_X + 20u, DUB_UI_KNOB_Y);
}

/* the kill tiles: S B / M T; a killed band: solid red, an X */
static void dub_ui_kill(int force)
{
    static const char *const L[DUB_NBAND] = {"S", "B", "M", "T"};
    uint32_t kill = (dub_ctl.kill | dub_live.kill) & DUB_KILL_ALL, b, sig = kill + 16u;
    if (!force && sig == dub_ui.sig_kill)
        return;
    dub_ui.sig_kill = sig;
    cv_begin(DUB_UI_KILL_W, DUB_UI_KILL_H, DUB_UI_PLATE);
    cv_rect(0, 0, DUB_UI_KILL_W, 1, DUB_UI_EDGE);
    cv_rect(0, DUB_UI_KILL_H - 1, DUB_UI_KILL_W, 1, DUB_UI_EDGE);
    cv_rect(0, 0, 1, DUB_UI_KILL_H, DUB_UI_EDGE);
    cv_rect(DUB_UI_KILL_W - 1, 0, 1, DUB_UI_KILL_H, DUB_UI_EDGE);
    for (b = 0; b < DUB_NBAND; b++) {
        int32_t x = 1 + (int32_t)(b & 1u) * 14, y = 1 + (int32_t)(b >> 1) * 19;
        int k = (int)((kill >> b) & 1u);
        cv_rect(x + 1, y + 1, 12, 17, k ? DUB_UI_RED : DUB_UI_BG);
        dub_ui_text(x + 3, y + 1, &FONT_S, k ? "X" : L[b], k ? C_WHITE : DUB_UI_GREEN, k ? DUB_UI_RED : DUB_UI_BG);
    }
    dub_ui_send(DUB_UI_KILL_X, DUB_UI_KILL_Y);
}

/* the limiter: the gain on a 0 .. -12 dB scale; reduction pulls the bar left */
static void dub_ui_lim(int force)
{
    uint32_t g = dub_tel.lim_gr_q15, i, n = 0, sig;
    int32_t len;
    while (n < 24u && g < DUB_UI_DB[n + 1u])
        n++;                                                    /* the reduction, half dB */
    len = DUB_UI_LIM_W - 2 - (int32_t)n * (DUB_UI_LIM_W - 2) / 24;
    sig = n + 1u;
    if (!force && sig == dub_ui.sig_lim)
        return;
    dub_ui.sig_lim = sig;
    cv_begin(DUB_UI_LIM_W, DUB_UI_LIM_H, DUB_UI_BG);
    cv_rect(0, 0, DUB_UI_LIM_W, 9, DUB_UI_PLATE);
    for (i = 0; i < (uint32_t)len; i += 3u)                     /* LED segments: green, gold reducing, red past 3 dB */
        cv_rect(1 + (int32_t)i, 2, 2, 5, !n ? DUB_UI_GREEN : n < 6u ? DUB_UI_GOLD : DUB_UI_RED);
    for (i = 0; i <= 4u; i++)                                   /* the ticks: 0 -3 -6 -9 -12 dB, right to left */
        cv_rect(DUB_UI_LIM_W - 2 - (int32_t)i * (DUB_UI_LIM_W - 2) / 4, 10, 1, 3, DUB_UI_GRAY);
    dub_ui_send(DUB_UI_LIM_X, DUB_UI_LIM_Y);
}

/* the scope: the strobe border, the tape vortex, the SUB / TREBLE bars */
static void dub_ui_scope(int force, int tick)
{
    int32_t lv = dub_ui_lvl(dub_tel.echo_env), r0 = 4 + (lv * 24 >> 8), sub = dub_ui_lvl(dub_tel.band_pk[DUB_SUB]),
            tre = dub_ui_lvl(dub_tel.band_pk[DUB_TREBLE]), wow = dub_punch_now() == DUB_PX_WOW, k;
    int siren = dub_ui_siren_open();
    uint32_t ph = dub_tel.siren_ph, sig;
    uint16_t strobe;
    if (tick && (lv || wow))
        dub_ui.rot += 8u + (uint32_t)(lv >> 4);                 /* the reels turn faster with more echo */
    {   /* the strobe: dark .. bright, a triangle of the LFO's phase */
        int32_t t = ph < 32768u ? (int32_t)(ph >> 7) : (int32_t)((65535u - ph) >> 7);
        strobe = siren ? dub_ui_mix(DUB_UI_STROBE_LO, DUB_UI_STROBE_HI, t) : DUB_UI_PLATE;
    }
    sig = (uint32_t)lv | (uint32_t)(sub >> 3) << 9 | (uint32_t)(tre >> 3) << 15 | (uint32_t)strobe << 21 |
          (uint32_t)wow << 31;
    sig ^= dub_ui.rot * 2654435761u;
    if (wow)
        sig ^= dub_ui.lfsr;                                     /* (shaking: a new picture every frame) */
    if (!force && sig == dub_ui.sig_scope)
        return;
    dub_ui.sig_scope = sig;
    cv_begin(DUB_UI_SCOPE_S, DUB_UI_SCOPE_S, strobe);
    cv_rect(4, 4, 64, 64, RGB(6, 8, 6));                         /* the 64x64 scope inside the border */
    for (k = 8; k < 64; k += 8) {                               /* the graticule */
        cv_rect(4 + k, 4, 1, 64, RGB(18, 24, 18));
        cv_rect(4, 4 + k, 64, 1, RGB(18, 24, 18));
    }
    cv_rect(5, 66 - (sub * 60 >> 8), 3, sub * 60 >> 8, sub > 230 ? DUB_UI_RED : DUB_UI_GREEN);   /* SUB, TREBLE */
    cv_rect(64, 66 - (tre * 60 >> 8), 3, tre * 60 >> 8, tre > 230 ? DUB_UI_RED : DUB_UI_GREEN);
    {   /* the vortex: a ring at the echo's level and a spiral arm winding into it, turning */
        int32_t cx = 36, cy = 36, px = 0, py = 0, qx = 0, qy = 0;
        uint32_t i;
        uint16_t ring = lv ? dub_ui_mix(DUB_UI_GOLD, C_WHITE, lv >> 2) : DUB_UI_GRAY;
        for (i = 0; i <= 48u; i++) {
            uint32_t a = (i * 1024u / 48u + dub_ui.rot) & 1023u;
            int32_t rr = r0, sr = r0 * (int32_t)(48u - i) / 48, x, y, sx, sy;
            if (wow) {                                          /* tape speed: the ring shakes */
                dub_ui.lfsr = dub_ui.lfsr * 1664525u + 1013904223u;
                rr += (int32_t)(dub_ui.lfsr >> 29) - 3;
            }
            x = cx + ((SINE[a] * rr + 16384) >> 15);
            y = cy - ((SINE[(a + 256u) & 1023u] * rr + 16384) >> 15);
            sx = cx + ((SINE[(a * 2u) & 1023u] * sr + 16384) >> 15);   /* (the arm turns twice as fast) */
            sy = cy - ((SINE[(a * 2u + 256u) & 1023u] * sr + 16384) >> 15);
            if (i) {
                cv_line(px, py, x, y, ring);
                cv_line(qx, qy, sx, sy, dub_ui_mix(DUB_UI_GOLD, DUB_UI_STROBE_LO, (int32_t)(i * 4u)));
            }
            px = x, py = y, qx = sx, qy = sy;
        }
        dub_ui_disc(cx, cy, 2, ring);                           /* the hub */
    }
    dub_ui_send(DUB_UI_SCOPE_X, DUB_UI_SCOPE_Y);
}

/* the values: drive, feedback, siren, echo, the flags, the punch slot, the MIDI channel */
static void dub_ui_value(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                         uint16_t c, int force)
{
    uint32_t sig = (uint32_t)c * 31u, k;
    for (k = 0; s[k]; k++)
        sig = sig * 131u + (uint8_t)s[k];
    sig += 1u;
    if (!force && sig == dub_ui.sig_txt[slot])
        return;
    dub_ui.sig_txt[slot] = sig;
    cv_begin(w, f->h, DUB_UI_BG);
    dub_ui_text(0, 0, f, s, c, DUB_UI_BG);
    dub_ui_send(x, y);
}

static void dub_ui_values(int force)
{
    char b[24];
    int32_t v;
    int8_t p;
    v = dub_ctl.drive_q12;                                      /* X1.0 .. X8.0 */
    b[0] = 'X';
    fmt_fix(b + 1, (v * 10 + 2048) / 4096, 1);
    dub_ui_value(0, 32, 106, 48, &FONT_S, b, DUB_UI_GOLD, force);
    v = (dub_ctl.echo_fb_q12 * 100 + 2048) / 4096;              /* 0.00 .. 1.25 (fmt_fix drops the leading 0) */
    b[0] = (char)('0' + v / 100), b[1] = '.', b[2] = (char)('0' + v / 10 % 10), b[3] = (char)('0' + v % 10), b[4] = 0;
    dub_ui_value(1, 32, 142, 48, &FONT_S, b, dub_ctl.echo_fb_q12 > DUB_Q12_ONE ? DUB_UI_RED : DUB_UI_GOLD, force);
    v = dub_ctl.siren_note_q8 >> 8;                             /* D5 */
    str_cpy(b, DUB_UI_NOTE[(uint32_t)v % 12u], sizeof b);
    fmt_int(b + str_len(b), v / 12 - 1);
    dub_ui_value(2, 160, 106, 48, &FONT_S, b, dub_ui_siren_open() ? DUB_UI_STROBE_HI : DUB_UI_GOLD, force);
    if (dub_ctl.echo_sync)
        str_cpy(b, DUB_UI_DLY[dub_ctl.echo_div % NDIV_DLY], sizeof b);
    else
        fmt_int(b, dub_ctl.echo_ms);
    dub_ui_value(3, 160, 142, 48, &FONT_S, b, DUB_UI_GOLD, force);
    str_cpy(b, dub_ctl.echo_to_rev ? "E>R " : "    ", sizeof b);   /* the switches (pads 11..14) */
    str_cpy(b + 4, dub_ctl.bypass ? "BYP " : "    ", sizeof b - 4);
    str_cpy(b + 8, dub_ctl.siren_sync ? "CLK " : "    ", sizeof b - 8);
    str_cpy(b + 12, dub_ctl.sub_arm ? "ARM" : "   ", sizeof b - 12);
    dub_ui_value(4, 36, 166, 168, &FONT_S, b, dub_ctl.bypass ? DUB_UI_RED : DUB_UI_GREEN, force);
    p = (int8_t)dub_punch_now();                                /* the punch slot, large */
    dub_ui_value(5, 32, 186, 176, &FONT_L, p >= 0 && p < (int8_t)DUB_NSLOT ? DUB_UI_PUNCH[(uint8_t)p] : "",
                 DUB_UI_RED, force);
    str_cpy(b, "MIDI CH ", sizeof b);
    if (dub_ctl.midi_ch)
        fmt_int(b + str_len(b), dub_ctl.midi_ch);
    else
        str_cpy(b + str_len(b), "OFF", sizeof b - str_len(b));
    dub_ui_value(6, 32, 222, 112, &FONT_S, b, DUB_UI_GRAY, force);
    dub_ui_value(7, 152, 222, 24, &FONT_S, "SUB", dub_tel.sub_on ? DUB_UI_GREEN : DUB_UI_PLATE, force);   /* the lamps */
    dub_ui_value(8, 184, 222, 24, &FONT_S, "USB", dub_tel.usb_in_live ? DUB_UI_GREEN : DUB_UI_PLATE, force);
}

/* ------------------------------------------------------------------ the page --- */
/* ui_draw.c: one frame. force: the whole page (opened, or back from another screen) */
static void dub_tower_draw(int force)
{
    uint32_t b;
    int tick = force || fm1_ms - dub_ui.last_ms >= DUB_UI_FRAME_MS;
    dub_ui.bytes = 0;
    if (force)
        dub_ui_plate();
    dub_ui_knob(force);
    dub_ui_kill(force);
    dub_ui_values(force);
    if (!tick)
        return;                                                 /* the moving parts: 30 a second */
    dub_ui.last_ms = fm1_ms;
    for (b = 0; b < DUB_NBAND; b++)
        dub_ui_driver(b, force);
    dub_ui_lim(force);
    dub_ui_scope(force, 1);
}

/* ui_input.c: a knob turned on the tower (s detents): KNOB 1 the big knob, 2 drive, 3 echo feedback, 4 siren */
static void dub_tower_knob(uint32_t k, int32_t s)
{
    if (!s)
        return;
    switch (k) {
    case 0:
        dub_ctl.tubby_step = (uint8_t)clamp((int32_t)dub_ctl.tubby_step + s, 0, DUB_TUBBY_NSTEP - 1);   /* a step a detent */
        dub_ctl.tubby_on = 1;                                   /* (the knob turns the filter in) */
        break;
    case 1: dub_ctl.drive_q12 = (int16_t)clamp(dub_ctl.drive_q12 + s * 256, DUB_Q12_ONE, DUB_DRIVE_MAX); break;
    case 2: dub_ctl.echo_fb_q12 = (int16_t)clamp(dub_ctl.echo_fb_q12 + s * 82, 0, DUB_FB_MAX); break;
    case 3: dub_ctl.siren_note_q8 = (int16_t)clamp(dub_ctl.siren_note_q8 + s * 256, 24 << 8, 108 << 8); break;
    default: break;
    }
}

/* the FX layer (Phase 4.2): pad i (0..15) is on: a kill, the punch slot, a switch (ui_layers.c, ui_input.c) */
static int dub_pad_lit(uint32_t i)
{
    if (i < DUB_NBAND)
        return (int)(((dub_ctl.kill | dub_live.kill) >> i) & 1u);
    if (i < 10u)
        return dub_punch_now() == (int32_t)(DUB_PX_ECHO_THROW + (int32_t)(i - 4u));
    switch (i) {
    case 10: return dub_ctl.echo_to_rev;
    case 11: return dub_ctl.bypass;
    case 12: return dub_ctl.siren_sync;
    case 13: return dub_ctl.sub_arm;
    default: return 0;
    }
}

/* the FX layer's screen (ui_layers.c): the pads' names, the knobs' names and values (lower case, as the layers) */
static const char *const DUB_PAD_NAME[16] = {"kill s", "kill b", "kill m", "kill t", "throw", "freeze", "wow",
                                             "stutt", "siren", "crash", "e>rev", "bypass", "clk", "bloom", "-", "-"};
static void dub_lower(char *s)                          /* (the layers' lower case) */
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
}
static void dub_fx_knobs(char v[4][10], const char **lab, int32_t *ratio)
{
    uint32_t step = dub_ctl.tubby_on ? dub_ctl.tubby_step % DUB_TUBBY_NSTEP : 0u;
    int32_t f, n = dub_ctl.siren_note_q8 >> 8;
    lab[0] = "hpf", lab[1] = "drive", lab[2] = "fdbk", lab[3] = "siren";
    str_cpy(v[0], DUB_UI_HPF[step], 10);
    dub_lower(v[0]);
    v[1][0] = 'x';
    fmt_fix(v[1] + 1, (dub_ctl.drive_q12 * 10 + 2048) / 4096, 1);
    f = (dub_ctl.echo_fb_q12 * 100 + 2048) / 4096;
    v[2][0] = (char)('0' + f / 100), v[2][1] = '.', v[2][2] = (char)('0' + f / 10 % 10), v[2][3] = (char)('0' + f % 10);
    v[2][4] = 0;
    str_cpy(v[3], DUB_UI_NOTE[(uint32_t)n % 12u], 10);
    fmt_int(v[3] + str_len(v[3]), n / 12 - 1);
    dub_lower(v[3]);
    ratio[0] = (int32_t)step * 1000 / 10;
    ratio[1] = (dub_ctl.drive_q12 - DUB_Q12_ONE) * 1000 / (DUB_DRIVE_MAX - DUB_Q12_ONE);
    ratio[2] = dub_ctl.echo_fb_q12 * 1000 / DUB_FB_MAX;
    ratio[3] = (n - 24) * 1000 / 84;
}

static void dub_tower_open(uint32_t page, uint32_t home)   /* (the page under it: from the menu, TRACKS) */
{
    dub_ui.on = 1;
    dub_ui.page = (uint8_t)page;
    dub_ui.home = (uint8_t)home;
}
static void dub_tower_close(void)
{
    dub_ui.on = 0;
}

#endif /* ENABLE_DUB_PREAMP */
