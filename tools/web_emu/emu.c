/* SPDX-License-Identifier: GPL-3.0-only */
/* The dub preamp FM-1 in the browser (Phase 7.2): the whole firmware, SLOOP's engine, its UI (every page, the
 * layers, the menu, the Mix grid) and the dub preamp (the CONTROL TOWER, the FX pads, the speaker EQ), on the host
 * build the tests use (tests/hostsim.c with ENABLE_DUB_PREAMP, the doubles of tests/ui_pages_test.c), compiled to
 * WebAssembly by tools/web_emu/build_emu.sh (Emscripten in Docker); tools/web_emu/app.js drives it.
 *
 * The design is Hortator's simulator (DEADACTIVE/hortator tests/sim_core.c, sim_host.h; GPL-3.0): the page only
 * feeds the hardware's raw events and reads its outputs; the firmware's own code decodes them.
 *   emu_render(n)            THE CLOCK. n stereo frames at 44.1 kHz (n <= EMU_MAXF), a pointer to 2n floats L R L R.
 *                            The audio ISR's work (mix_block, CTL frames at a time, the keys scanned in it as seq.c
 *                            does) and main.c's loop body (dub_mq_apply, ui_input, ui_leds, ui_draw) every 15 ms of
 *                            audio, between blocks as the main loop runs between ISRs. fm1_ms follows the sample
 *                            count: time stops when the audio does. -> the number of UI frames run in emu_ui_frames()
 *   emu_btn(label, down)     a button (label B_FX .. B_OCTUP, panel.c): its matrix bit in fm1_in.buttons through
 *                            the panel table, its press edge latched until ui_input takes it (fm1_input_edges)
 *   emu_key(n, down)         a key: n = 0 (F3) .. 26 (G5), fm1_in.notes
 *   emu_enc(role, steps)     an encoder (EN_SELECT .. EN_K4), + = clockwise: detents for fm1_enc_take
 *   emu_master(raw)          the MASTER pot, ADC 0 .. 1023, through main.c's filter and curve
 * outputs: emu_screen() (240 x 240 RGB565, byte-swapped as the panel takes it), emu_leds() (bit b = button label
 * b), emu_key_leds() (bit n = key n), emu_playing(), emu_band(b) (the preamp's band meters: the page's speaker
 * cones), emu_keymap() (the LED matrix table, checked against hal/ by emu_smoke.mjs) and emu_state(k), a read-only
 * probe for the tests (as Hortator's felucca_dbg): nothing in the page writes a parameter.
 * Floats appear only here, at the edge (the firmware has none). */
#include <stdint.h>
#include <emscripten/emscripten.h>
#define ENABLE_DUB_PREAMP
#define FELUCCA_ARRANGER 1
#define main hostsim_main                               /* SLOOP on the host: as the tests build it */
#include "../../tests/hostsim.c"
#undef main

/* ---- the LCD: a 240 x 240 framebuffer, as the panel takes it (byte-swapped) */
static uint16_t screen[240 * 240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                screen[(y + j) * 240u + x + i] = p[j * w + i];
}
#include "../../firmware/src/gfx.c"
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            screen[j * 240u + i] = swap16(c);
}

/* ---- input (hal/fm1_input.h's API): levels set by the page, press edges latched until the UI takes them */
static int32_t enc_acc[7];
static uint32_t btn_edges, notes_seen;
static uint32_t fm1_ticks(void) { return fm1_ms * 1000u * 24u; }
#define FM1_TICKS_PER_US 24u
static int32_t fm1_enc_take(uint32_t e) { int32_t s = enc_acc[e % 7u]; enc_acc[e % 7u] = 0; return s; }
static uint32_t fm1_input_edges(int x) { uint32_t e = btn_edges; (void)x; btn_edges = 0; return e; }
static uint32_t fm1_input_note_edges(void) { uint32_t e = fm1_in.notes & ~notes_seen; notes_seen = fm1_in.notes; return e; }
static void fm1_wdt_feed(void) { fm1_ms += 1000u; }   /* (a busy-wait: no input can come, it times out) */
static int32_t fm1_adc_read(int c) { (void)c; return -1; }

/* ---- the LEDs: hal/fm1_input.h's matrix (copied; emu_smoke.mjs checks it against hal/) */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_bg[FM1_NCOL];
static volatile uint16_t fm1_led_bg_ns;
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                fm1_led[p] = on ? (uint8_t)(fm1_led[p] | 1u << r) : (uint8_t)(fm1_led[p] & ~(1u << r));
}

/* ---- the rest of felucca.c's world the UI reaches: no flash in the browser (projects, presets, settings stay in
 * RAM for the visit), the visualiser's scope fed by the clock below */
static struct { uint32_t magic, stage, page, home, ui_frames; } felucca_dbg;
#define FELUCCA_ICONS 1
#define SCOPE_N 512u
static int16_t scope_buf[SCOPE_N], scope_bufr[SCOPE_N];
static uint32_t scope_w;
#include "../../firmware/src/panel.c"
#include "../../firmware/src/ui.c"
static int project_used(uint32_t i) { (void)i; return 0; }
static void project_save(uint32_t i) { (void)i; ui_message("NO FLASH IN THE BROWSER"); }
static void project_load(uint32_t i) { (void)i; }
static void arrangement_save(void) { ui_message("NO FLASH IN THE BROWSER"); }
static uint32_t arrangement_ready(void) { return 0; }
static void arrangement_apply(uint32_t s) { (void)s; }
static uint32_t section_bars(uint32_t s) { (void)s; return 1u; }
static void song_backup(void) {}
static void song_restore(void) {}
static void section_store(uint32_t s) { live_sec = (int8_t)s; }
static void section_load(uint32_t s) { live_sec = (int8_t)s; }
static int up_used(uint32_t k) { (void)k; return 0; }
static int up_load(uint32_t k) { (void)k; return 0; }
static uint32_t up_count(void) { return 0; }
static uint32_t up_nth(uint32_t n) { return n; }
static uint32_t up_rank(uint32_t s) { return s; }
static void up_name(uint32_t k, char *b) { (void)k; b[0] = 0; }
static void up_slot_label(char *b, uint32_t k) { fmt_int(b, (int32_t)k + 1); }
static void up_ui(uint32_t op, uint32_t k) { (void)op; (void)k; }
static void settings_save(void) {}
#include "../../firmware/src/ui_song.c"
#include "../../firmware/src/ui_studio.c"
#include "../../firmware/src/icons.c"
#include "../../firmware/src/dub_preamp/dub_ui_dashboard.c"
#include "../../firmware/src/ui_draw.c"
#include "../../firmware/src/ui_vis.c"
#include "../../firmware/src/ui_layers.c"
#include "../../firmware/src/ui_menu.c"
#include "../../firmware/src/dub_preamp/dub_menu_extensions.c"
#include "../../firmware/src/ui_input.c"

/* ---- the clock (Hortator's sim_render): the audio, and the main loop every 15 ms of it */
#define EMU_MAXF 4096u
#define EMU_FRAME_SAMPLES (FS * 15u / 1000u)          /* main.c: ~60 UI frames a second at most */
static float emu_out[2u * EMU_MAXF];
static int32_t emu_blk[2u * CTL];
static uint32_t emu_blk_pos = CTL;                      /* frames of emu_blk already handed out */
static uint64_t emu_samples, emu_next_frame;
static uint32_t emu_frames_run;
static int32_t emu_knob = 800 * 16;                     /* main.c's MASTER filter state */
static uint32_t emu_master_raw = 800;

static void emu_main_loop(void)                         /* main.c's loop body, minus the hardware */
{
    int32_t a = (int32_t)emu_master_raw;
    uint32_t k10;
    emu_knob += (a * 16 - emu_knob) / 8;
    k10 = (uint32_t)(emu_knob / 16);
    song.master_q12 = (k10 * k10) >> 8;                 /* 0 .. ~4096 */
    dub_mq_apply();                                     /* the preamp's MIDI CCs and pad toggles */
    felucca_dbg.ui_frames++;
    felucca_dbg.page = ui.page;
    felucca_dbg.home = ui.home;
    ui_input();
    ui_leds();
    ui_draw();
    emu_frames_run++;
}

/* the riddim the console boots with (no flash: nothing else to load): a roots bass line, the skank, the drums */
static void emu_preset(track_t *t, const char *eng, const char *name)
{
    uint32_t e, p;
    for (e = 0; e < NENGINES; e++) {
        if (!strstr(ENGINES[e]->name, eng))
            continue;
        for (p = 0; p < ENGINES[e]->npresets; p++)
            if (strstr(ENGINES[e]->presets[p].name, name)) {
                host_preset(t, e, p);
                return;
            }
        host_preset(t, e, 0);
        return;
    }
}
static void emu_riddim(void)
{
    static const uint8_t BASS[32] = {45, 0, 0, 45, 0, 0, 48, 0, 50, 0, 0, 52, 0, 50, 48, 0,
                                     45, 0, 0, 45, 0, 0, 43, 0, 45, 0, 0, 0, 40, 0, 43, 0};
    static const uint8_t AM[3] = {57, 60, 64}, DM[3] = {57, 62, 65};
    track_t *bass = &trk[0], *skank = &trk[1], *lead = &trk[2], *dr = TDRUM;
    uint32_t i;
    host_tracks_init();
    song.g[G_BPM] = 75;
    emu_preset(bass, "ANALOG", "BASS");
    emu_preset(skank, "WHEEL", "");
    emu_preset(lead, "DIGITAL", "");
    bass->p[P_SLEN] = 32;
    for (i = 0; i < 32u; i++) {
        uint8_t n = BASS[i];
        put_step(bass, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, 0);
    }
    skank->p[P_SLEN] = 32;
    skank->p[P_SGATE] = 30;
    for (i = 0; i < 32u; i++)
        put_step(skank, i, i % 4u == 2u ? 3u : 0u, i < 16u ? AM : DM, i % 4u == 2u ? ST_NOTE : ST_REST, 0);
    lead->p[P_SLEN] = 16;
    for (i = 0; i < 16u; i++) {                         /* one drop (kick and rim on beat 3), hats */
        uint8_t n[3];
        uint32_t k = 0;
        if (i == 8u)
            n[k++] = 36, n[k++] = 37;
        if (i % 2u == 0u)
            n[k++] = 42;
        put_step(dr, i, k, n, k ? ST_NOTE : ST_REST, i == 8u ? SF_ACCENT : 0u);
    }
}

/* power-on: main.c's boot order (persist_boot has no flash here: the settings as shipped), the riddim playing */
EMSCRIPTEN_KEEPALIVE void emu_init(void)
{
    bank_resolve();
    settings_init();                                    /* ROOTS DARK, SPEAKER EQ FLAT */
    panel_init();
    layers_init();
    dub_ctl_defaults();
    dub_ctl.echo_send_q15 = 9000;                       /* (a little echo on everything, to hear the line) */
    dub_ctl.rev_send_q15 = 4000;
    emu_riddim();
    memset(&felucca_dbg, 0, sizeof felucca_dbg);
    emu_knob = (int32_t)emu_master_raw * 16;
    emu_main_loop();                                    /* settles MASTER, draws the first screen */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.force = 1;
    go_home();
    transport_req = 1;
    emu_next_frame = emu_samples + EMU_FRAME_SAMPLES;
}

EMSCRIPTEN_KEEPALIVE float *emu_render(uint32_t n)
{
    uint32_t i;
    if (n > EMU_MAXF)
        n = EMU_MAXF;
    for (i = 0; i < n; i++) {
        if (emu_blk_pos >= CTL) {
            uint32_t k;
            if (emu_samples >= emu_next_frame) {        /* between blocks, as the main loop runs between ISRs */
                emu_main_loop();
                emu_next_frame += EMU_FRAME_SAMPLES;
            }
            mix_block(emu_blk, CTL);                    /* events, keys, parts, drums, buses, the preamp, MASTER */
            for (k = 1; k < CTL; k += 2u) {             /* the visualiser's scope, as audio.c fills it */
                scope_bufr[scope_w & (SCOPE_N - 1u)] = vis_tap[2u * k + 1u];
                scope_buf[scope_w++ & (SCOPE_N - 1u)] = vis_tap[2u * k];
            }
            emu_samples += CTL;
            fm1_ms = (uint32_t)(emu_samples * 1000u / FS);
            emu_blk_pos = 0;
        }
        emu_out[2u * i] = (float)emu_blk[2u * emu_blk_pos] * (1.0f / 32768.0f);
        emu_out[2u * i + 1u] = (float)emu_blk[2u * emu_blk_pos + 1u] * (1.0f / 32768.0f);
        emu_blk_pos++;
    }
    return emu_out;
}
EMSCRIPTEN_KEEPALIVE uint32_t emu_ui_frames(void) { return emu_frames_run; }

/* ---- the hardware's events */
EMSCRIPTEN_KEEPALIVE void emu_btn(uint32_t label, int down)
{
    uint32_t bit;
    if (label >= NB)
        return;
    bit = 1u << panel.btn[label];
    if (down) {
        btn_edges |= bit & ~fm1_in.buttons;
        fm1_in.buttons |= bit;
    } else {
        fm1_in.buttons &= ~bit;
    }
}
EMSCRIPTEN_KEEPALIVE void emu_key(uint32_t n, int down)
{
    if (n >= 27u)
        return;
    fm1_in.notes = down ? fm1_in.notes | 1u << n : fm1_in.notes & ~(1u << n);
}
EMSCRIPTEN_KEEPALIVE void emu_enc(uint32_t role, int32_t steps)
{
    if (role < NE)
        enc_acc[panel.enc[role]] += steps * panel.dir[role];
}
EMSCRIPTEN_KEEPALIVE void emu_master(uint32_t raw) { emu_master_raw = raw > 1023u ? 1023u : raw; }

/* ---- the outputs */
EMSCRIPTEN_KEEPALIVE uint16_t *emu_screen(void) { return screen; }
static int emu_led_lit(uint32_t id)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                return (fm1_led[p] >> r) & 1u;
    return 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t emu_leds(void)
{
    uint32_t b, m = 0;
    for (b = 0; b < NB; b++)
        m |= (uint32_t)emu_led_lit(panel.btn[b]) << b;
    return m;
}
EMSCRIPTEN_KEEPALIVE uint32_t emu_key_leds(void)
{
    uint32_t n, m = 0;
    for (n = 0; n < 27u; n++)
        m |= (uint32_t)emu_led_lit(14u + n) << n;
    return m;
}
EMSCRIPTEN_KEEPALIVE int emu_playing(void) { return song.playing; }
EMSCRIPTEN_KEEPALIVE uint32_t emu_band(uint32_t b) { return dub_tel.band_pk[b & 3u]; }
EMSCRIPTEN_KEEPALIVE const int8_t *emu_keymap(void) { return &FM1_KEYMAP[0][0]; }

/* the tests' probe (read only): k -> a value of the firmware's state */
enum { ST_PAGE, ST_HOME, ST_MENU, ST_MENU_SEL, ST_LAYER, ST_LOWCUT, ST_TOWER, ST_LOCK, ST_USB_MUTE, ST_USB_SOLO,
       ST_KILL, ST_LATCHED, ST_SIREN_SOLO, ST_MI_LOWCUT, ST_LY_MIX, ST_LY_FX, ST_MASTER, ST_TRACKS_PAGE };
EMSCRIPTEN_KEEPALIVE int32_t emu_state(uint32_t k)
{
    switch (k) {
    case ST_PAGE: return ui.page;
    case ST_HOME: return ui.home;
    case ST_MENU: return ui.menu;
    case ST_MENU_SEL: return ui.menu_sel;
    case ST_LAYER: return ui.layer;
    case ST_LOWCUT: return fx_lowcut;
    case ST_TOWER: return dub_ui.on;
    case ST_LOCK: return dub_ui.lock;
    case ST_USB_MUTE: return dub_ctl.usb_mute;
    case ST_USB_SOLO: return dub_ctl.usb_solo;
    case ST_KILL: return (int32_t)dub_kill_now();
    case ST_LATCHED: return dub_live.latched;
    case ST_SIREN_SOLO: return dub_live.siren_solo;
    case ST_MI_LOWCUT: return MI_LOWCUT;
    case ST_LY_MIX: return LY_MIX;
    case ST_LY_FX: return LY_FX;
    case ST_MASTER: return (int32_t)song.master_q12;
    case ST_TRACKS_PAGE: return (int32_t)page_first(FAM_TRK);   /* (go_home's page: TRACKS) */
    default: return -1;
    }
}
