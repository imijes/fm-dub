/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the dub preamp's Phase 1 (firmware/src/dub_preamp/): the shared state's helpers, the DUB menu
 * rows, the settings record at 0xFB000 on a simulated NOR (erased, round trip, unchanged = no erase, torn write,
 * bit rot, out-of-range values, the performance state not saved). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define ENABLE_DUB_PREAMP
#define FELUCCA_FLASH 1
#define FS 44100

/* what the module takes from SLOOP (core.h, libc.c, gfx.c, ui.c, ui_menu.c, felucca.c) */
#define NPART 3
#define NDIV_SHORT 6u
#define NDIV_DLY 8u
enum { G_DRCH, G_COUNT };
static struct { int16_t g[G_COUNT]; } song;
static struct { uint8_t force, page, home; } ui;
static uint16_t pal[5] = {0, 1, 2, 3, 4};
#define C_DIM pal[1]
#define C_AMB pal[3]
#define C_HI pal[4]
enum { MI_DUBCH = 8, MI_DUBON, MI_DUBRST, MI_DUBTWR };
static int menu_closed, tower_opened;
static void menu_close(void) { menu_closed++; }
static void dub_tower_open(uint32_t page, uint32_t home) { (void)page, (void)home; tower_opened++; }
static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static void fmt_int(char *b, int32_t v) { snprintf(b, 4, "%d", (int)v); }
static const char *last_msg;
static void ui_message(const char *m) { last_msg = m; }

static uint8_t nor[0x100000];
static int fail_after = -1, erases;
static uint8_t flash_ok = 1;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); erases++; return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0)
        return -9;
    if (fail_after > 0)
        fail_after--;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x\n", off);
        exit(1);
    }
    if (off < 0xFB000u || off + n > 0xFC000u) {
        printf("write outside the dub sector at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#pragma GCC diagnostic ignored "-Wunused-function"       /* storage.c helpers this test does not call */
#include "../firmware/src/storage.c"
#define __attribute__(x)                                /* (.pool and the like: as hostsim.c) */
#include "../firmware/src/dub_preamp/dub_shared_state.h"
#include "../firmware/src/dub_preamp/dub_menu_extensions.c"

static int check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void)
{
    int bad = 0;
    uint16_t c;
    uint32_t seen = 0, cc, v, k;
    memset(nor, 0xFF, sizeof nor);
    song.g[G_DRCH] = 10;

    /* the helpers */
    dub_ctl_defaults();
    dub_ev_post(&dub_ctl.ev_crash);
    dub_ev_post(&dub_ctl.ev_crash);
    bad += check("events: two posts, two takes, then none",
                 dub_ev_take(&dub_ctl.ev_crash, &seen) && dub_ev_take(&dub_ctl.ev_crash, &seen) &&
                 !dub_ev_take(&dub_ctl.ev_crash, &seen));
    for (k = 0; k < DUB_MQ + 5u; k++)
        dub_mq_put(k, k + 1u);
    for (k = 0; dub_mq_get(&cc, &v); k++)
        if (cc != k || v != k + 1u)
            break;
    bad += check("CC queue: in order, a full queue drops", k == DUB_MQ);
    dub_ctl.kill = DUB_KILL_SUB;
    dub_live.kill = DUB_KILL_TREBLE;
    dub_live.punch = DUB_PX_DUCK;
    bad += check("kills: buttons OR MIDI; MIDI punch wins",
                 dub_kill_now() == (DUB_KILL_SUB | DUB_KILL_TREBLE) && dub_punch_now() == DUB_PX_DUCK);
    dub_live.kill = 0;
    dub_live.punch = DUB_PX_NONE;

    /* boot on erased flash: the defaults, MIDI 11 */
    dub_ctl.midi_ch = 3;
    dub_persist_boot();
    bad += check("erased flash: defaults, MIDI CH 11", dub_ctl.midi_ch == 11u && dub_ctl.echo_ms == 375);
    bad += check("erased flash: counts as changed (first save writes)", dub_persist_changed());

    /* the menu */
    bad += check("menu: MIDI CH shows 11", !strcmp(dub_mi_value(MI_DUBCH, &c), "11") && c == C_HI);
    dub_mi_set(MI_DUBCH, -1);
    bad += check("menu: knob left -> 10, amber (the drum channel)",
                 dub_ctl.midi_ch == 10u && !strcmp(dub_mi_value(MI_DUBCH, &c), "10") && c == C_AMB);
    for (k = 0; k < 20u; k++)
        dub_mi_set(MI_DUBCH, -1);
    bad += check("menu: stops at OFF", dub_ctl.midi_ch == 0u && !strcmp(dub_mi_value(MI_DUBCH, &c), "OFF"));
    for (k = 0; k < 20u; k++)
        dub_mi_set(MI_DUBCH, 1);
    bad += check("menu: stops at 16", dub_ctl.midi_ch == 16u);
    dub_mi_set(MI_DUBCH, 0);
    bad += check("menu: OCT+ steps round 16 -> OFF", dub_ctl.midi_ch == 0u);
    dub_ctl.midi_ch = 2;
    dub_mi_value(MI_DUBCH, &c);
    bad += check("menu: a part's channel is amber", c == C_AMB);
    dub_mi_set(MI_DUBON, 0);
    bad += check("menu: PREAMP OCT+ -> BYPASS", dub_ctl.bypass == 1u && !strcmp(dub_mi_value(MI_DUBON, &c), "BYPASS"));
    dub_mi_set(MI_DUBON, 1);
    bad += check("menu: PREAMP right -> ON", dub_ctl.bypass == 0u);
    bad += check("menu: other items are not ours", !dub_mi_value(0, &c) && !dub_mi_set(0, 1));
    {
        uint32_t s0 = dub_menu_sig();
        dub_ctl.midi_ch = 5;
        bad += check("menu: the signature follows the channel", dub_menu_sig() != s0);
    }
    dub_ctl.bypass = 1;
    dub_mi_set(MI_DUBRST, 1);
    bad += check("menu: DEFAULTS ignores a knob", dub_ctl.midi_ch == 5u && dub_ctl.bypass == 1u);
    dub_mi_set(MI_DUBRST, 0);
    bad += check("menu: DEFAULTS on OCT+", dub_ctl.midi_ch == 11u && !dub_ctl.bypass && last_msg && ui.force);
    dub_mi_set(MI_DUBTWR, 1);
    bad += check("menu: CONTROL TOWER ignores a knob", !menu_closed && !tower_opened);
    dub_mi_set(MI_DUBTWR, 0);
    bad += check("menu: CONTROL TOWER on OCT+: the menu closes, the tower opens", menu_closed == 1 && tower_opened == 1);
    bad += check("menu: CONTROL TOWER shows as an action", !strcmp(dub_mi_value(MI_DUBTWR, &c), "") && c == C_DIM);

    /* save, boot again */
    dub_ctl.midi_ch = 7;
    dub_ctl.echo_ms = 900;
    dub_ctl.echo_fb_q12 = DUB_FB_MAX;
    dub_ctl.siren_wave = DUB_WAVE_SAW;
    dub_ctl.tubby_step = 10;
    dub_ctl.kill = DUB_KILL_ALL;              /* performance state: not saved */
    dub_ctl.sub_arm = 1;
    dub_ctl.punch = DUB_PX_STUTTER;
    erases = 0;
    bad += check("save: ok", dub_persist_save() == 0 && erases == 1);
    bad += check("save: then unchanged", !dub_persist_changed());
    bad += check("save again unchanged: no erase", dub_persist_save() == 0 && erases == 1);
    dub_ctl_defaults();
    dub_persist_boot();
    bad += check("boot: the settings back",
                 dub_ctl.midi_ch == 7u && dub_ctl.echo_ms == 900 && dub_ctl.echo_fb_q12 == DUB_FB_MAX &&
                 dub_ctl.siren_wave == DUB_WAVE_SAW && dub_ctl.tubby_step == 10u);
    bad += check("boot: nothing killed, nothing armed, no punch",
                 !dub_ctl.kill && !dub_ctl.sub_arm && dub_ctl.punch == DUB_PX_NONE);
    bad += check("boot: counts as saved", !dub_persist_changed());

    /* a torn write: the header never lands -> defaults at the next boot */
    dub_ctl.midi_ch = 9;
    fail_after = 1;                           /* the payload goes, the header does not */
    bad += check("torn write: reported", dub_persist_save() != 0);
    fail_after = -1;
    bad += check("torn write: still unsaved", dub_persist_changed());
    dub_persist_boot();
    bad += check("torn write: boot falls back to the defaults", dub_ctl.midi_ch == 11u);

    /* bit rot in the payload */
    dub_ctl.midi_ch = 4;
    dub_persist_save();
    nor[0xFB000u + DUB_REC_PAYLOAD + 17u] ^= 0x10u;
    dub_persist_boot();
    bad += check("bit rot: CRC rejects, defaults", dub_ctl.midi_ch == 11u);

    /* out-of-range values in a valid record come back inside */
    {
        dub_persist_t p;
        dub_rec_hdr_t h;
        memset(&p, 0, sizeof p);
        p.midi_ch = 200;
        p.echo_ms = 30000;
        p.echo_fb_q12 = 32767;
        p.drive_q12 = -5;
        p.tubby_step = 99;
        p.siren_wave = 9;
        p.echo_div = 50;
        st_erase(0xFB000u);
        memcpy(dub_rec_buf, &p, sizeof p);
        st_prog(0xFB000u + DUB_REC_PAYLOAD, dub_rec_buf, sizeof p);
        h.magic = DUB_REC_MAGIC;
        h.ver = 1;
        h.len = sizeof p;
        h.crc = st_crc32(&p, sizeof p);
        h.hcrc = st_crc32(&h, sizeof h - 4u);
        st_prog(0xFB000u, &h, sizeof h);
        dub_persist_boot();
        bad += check("clamped: MIDI 16, echo 1200 ms, fb 1.25, drive x1",
                     dub_ctl.midi_ch == 16u && dub_ctl.echo_ms == (int16_t)DUB_ECHO_MAX_MS &&
                     dub_ctl.echo_fb_q12 == DUB_FB_MAX && dub_ctl.drive_q12 == DUB_Q12_ONE);
        bad += check("clamped: step 10, wave SAW, div 7",
                     dub_ctl.tubby_step == 10u && dub_ctl.siren_wave == DUB_WAVE_SAW && dub_ctl.echo_div == 7u);
        bad += check("clamped: written back at the next save", dub_persist_changed());
    }

    /* no flash: defaults, nothing written */
    flash_ok = 0;
    erases = 0;
    dub_persist_boot();
    bad += check("no flash: defaults, save does nothing",
                 dub_ctl.midi_ch == 11u && dub_persist_save() == 0 && erases == 0);

    printf(bad ? "%d FAILED\n" : "all ok\n", bad);
    return bad != 0;
}
