/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP boot splash: the logo (tools/gen_logo.py -> sloop_logo.h: 16 colours, RLE runs of
 * (run - 1) << 4 | colour), drawn into the canvas in bands of at most 120 rows.
 * ENABLE_DUB_PREAMP: a text splash instead (the flash is full; the logo is 4.6 kB): the sound system's colours,
 * what it is, where it comes from. SLOOP's and Felucca's credits are on the ABOUT page as ever. */
#ifdef ENABLE_DUB_PREAMP
static void sloop_splash(void)
{
    static const char *const L[3] = {"M-VAVE FM-1", "SOUND SYSTEM PREAMP", "DUB PREAMP FORK V1.0A"};
    static const uint16_t BAR[3] = {RGB(214, 30, 30), RGB(250, 196, 0), RGB(20, 150, 60)};
    uint32_t i;
    lcd_fill(0, 0, 240, 240, 0x10A2u);                  /* the CONTROL TOWER's steel */
    for (i = 0; i < 3u; i++)
        lcd_fill(0, 40u + 8u * i, 240, 8, BAR[i]);      /* red, gold, green */
    for (i = 0; i < 3u; i++)
        lcd_fill(0, 176u + 8u * i, 240, 8, BAR[2u - i]);
    cv_begin(240, 96, 0x10A2u);                         /* (dub_ui_text: blended onto the steel, no black band) */
    dub_ui_text(120 - text_w(&FONT_S, L[0]) / 2, 0, &FONT_S, L[0], RGB(150, 150, 156), 0x10A2u);
    dub_ui_text(120 - text_w(&FONT_L, "DUB") / 2, 22, &FONT_L, "DUB", RGB(250, 196, 0), 0x10A2u);
    dub_ui_text(120 - text_w(&FONT_S, L[1]) / 2, 58, &FONT_S, L[1], RGB(220, 220, 226), 0x10A2u);
    dub_ui_text(120 - text_w(&FONT_S, L[2]) / 2, 78, &FONT_S, L[2], RGB(214, 30, 30), 0x10A2u);
    cv_blit(0, 72);
    cv_begin(240, 16, 0x10A2u);
    dub_ui_text(120 - text_w(&FONT_S, "based on SLOOP + Felucca") / 2, 0, &FONT_S, "based on SLOOP + Felucca",
                RGB(110, 110, 118), 0x10A2u);
    cv_blit(0, 214);
}
#else
#include "sloop_logo.h"
static void sloop_logo_draw(uint32_t y0)
{
    uint32_t r0;
    for (r0 = 0; r0 < SLOOP_SPLASH_H; r0 += 120u) {
        uint32_t rows = SLOOP_SPLASH_H - r0 > 120u ? 120u : SLOOP_SPLASH_H - r0;
        uint32_t lo = r0 * SLOOP_SPLASH_W, hi = (r0 + rows) * SLOOP_SPLASH_W, pos = 0, i;
        cv_begin(SLOOP_SPLASH_W, rows, C_BLACK);
        for (i = 0; i < sizeof SLOOP_SPLASH_RLE && pos < hi; i++) {
            uint32_t run = (SLOOP_SPLASH_RLE[i] >> 4) + 1u, c = SLOOP_SPLASH_RLE[i] & 15u, p;
            if (c && pos + run > lo)
                for (p = pos < lo ? lo : pos; p < pos + run && p < hi; p++)
                    cv_px[p - lo] = swap16(SLOOP_SPLASH_PAL[c]);
            pos += run;
        }
        cv_blit((240u - SLOOP_SPLASH_W) / 2u, y0 + r0);
    }
}

/* the boot screen: the logo, the version, where it comes from */
static void sloop_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    sloop_logo_draw(10);
    const char *v = FELUCCA_VERSION;
    if (v[0] == 'S' && v[1] == 'L' && v[5] == ' ')
        v += 6;                                         /* "SLOOP 1.0" -> "1.0" under the wordmark */
    cv_begin(240, 36, C_BLACK);
    cv_text(120 - text_w(&FONT_S, v) / 2, 0, &FONT_S, v, RGB(196, 196, 204));
    cv_text(120 - text_w(&FONT_S, "based on felucca") / 2, 18, &FONT_S, "based on felucca", RGB(96, 96, 104));
    cv_blit(0, 202);
}
#endif
