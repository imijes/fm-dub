/* SPDX-License-Identifier: GPL-3.0-only */
/* The dub preamp in the browser: SLOOP's real audio engine and the whole preamp chain (the host build the tests
 * use: tests/hostsim.c with ENABLE_DUB_PREAMP), the CONTROL TOWER drawn into a 240 x 240 RGB565 framebuffer, a
 * dub riddim on the sequencer. Compiled to WebAssembly by tools/web_emu/build_emu.sh (Emscripten in Docker);
 * tools/web_emu/app.js drives it:
 *
 *   emu_init()               the riddim (one drop, 75 BPM), the preamp as shipped, the transport playing
 *   emu_render(n)            n stereo frames at 44.1 kHz (n <= EMU_MAXF): a pointer to 2n floats, L R L R
 *   emu_frame(ms, force)     the main loop: ms on, the queued CCs / pad toggles applied, the tower drawn;
 *                            a pointer to the screen (240 x 240 RGB565, byte-swapped as the panel takes it)
 *   emu_pad(i, down)         pad i (0..15), as FX + a white key on the FM-1 (in the audio context, as seq.c)
 *   emu_knob(k, steps)       KNOB k (0..3) of the tower: the big knob, drive, echo feedback, siren pitch
 *   emu_note(note, on)       a note on track 3 (MIDI channel 3, as USB-MIDI would bring it)
 *   emu_play(on)             the transport
 *   emu_cc(cc, value)        a CC on the preamp's MIDI channel (dub_midi.c)
 *   emu_pads_lit(), emu_playing(), emu_band(b)   the page's lamps, its PLAY button, its cabinets' cones
 *
 * The browser's own clock and WebAudio pace it; floats appear only here, at the edge (the firmware has none). */
#include <stdint.h>
#include <emscripten/emscripten.h>
#define ENABLE_DUB_PREAMP
#define main hostsim_main                               /* SLOOP on the host: as the tests build it */
#include "../../tests/hostsim.c"
#undef main

static uint16_t screen[240 * 240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            screen[(y + j) * 240u + x + i] = p[j * w + i];
}
#include "../../firmware/src/gfx.c"
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            screen[(y + j) * 240u + x + i] = swap16(c);
}
#include "../../firmware/src/dub_preamp/dub_ui_dashboard.c"

#define EMU_MAXF 4096u
static float emu_out[2u * EMU_MAXF];
static int32_t emu_blk[2u * CTL];
static uint32_t emu_blk_pos = CTL;                      /* frames of emu_blk already given out */
static int emu_forced;

/* a factory preset by the engine's and the preset's names (the first that contains the text) */
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
    /* the bass (track 1): a roots line in A minor over two bars of eighths; 0 = rest */
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
    skank->p[P_SLEN] = 32;                              /* the skank: a short chord on every offbeat */
    skank->p[P_SGATE] = 30;
    for (i = 0; i < 32u; i++)
        put_step(skank, i, i % 4u == 2u ? 3u : 0u, i < 16u ? AM : DM, i % 4u == 2u ? ST_NOTE : ST_REST, 0);
    lead->p[P_SLEN] = 16;                               /* track 3: yours to play (emu_note) */
    for (i = 0; i < 16u; i++)                           /* the drums: one drop (kick and rim on beat 3), hats */
    {
        uint8_t n[3];
        uint32_t k = 0;
        if (i == 8u)
            n[k++] = 36, n[k++] = 37;
        if (i % 2u == 0u)
            n[k++] = 42;
        put_step(dr, i, k, n, k ? ST_NOTE : ST_REST, i == 8u ? SF_ACCENT : 0u);
    }
    song.master_q12 = 4096;
}

EMSCRIPTEN_KEEPALIVE void emu_init(void)
{
    emu_riddim();
    dub_ctl_defaults();
    dub_ctl.echo_send_q15 = 9000;                       /* (a little echo on everything, to hear the line) */
    dub_ctl.rev_send_q15 = 4000;
    dub_live.punch = DUB_PX_NONE;
    dub_tel.lim_gr_q15 = 32767;
    dub_tel.punch = 0xFFu;
    transport_req = 1;
    emu_forced = 0;
}

EMSCRIPTEN_KEEPALIVE float *emu_render(uint32_t n)
{
    uint32_t i;
    if (n > EMU_MAXF)
        n = EMU_MAXF;
    for (i = 0; i < n; i++) {
        if (emu_blk_pos >= CTL) {
            mix_block(emu_blk, CTL);                    /* events, the parts, the drums, the buses, the preamp, master */
            emu_blk_pos = 0;
        }
        emu_out[2u * i] = (float)emu_blk[2u * emu_blk_pos] * (1.0f / 32768.0f);
        emu_out[2u * i + 1u] = (float)emu_blk[2u * emu_blk_pos + 1u] * (1.0f / 32768.0f);
        emu_blk_pos++;
    }
    return emu_out;
}

EMSCRIPTEN_KEEPALIVE uint16_t *emu_frame(uint32_t ms, int force)
{
    fm1_ms += ms;
    dub_mq_apply();                                     /* (main.c: the CCs, the pads' toggles) */
    dub_tower_draw(force || !emu_forced);
    emu_forced = 1;
    return screen;
}

EMSCRIPTEN_KEEPALIVE void emu_pad(uint32_t i, int down)
{
    uint32_t keybit = 1u << (i & 15u);                  /* (a key per pad, as the white keys) */
    if (down)
        dub_pad_down(i & 15u, keybit);
    else
        dub_pad_up(keybit);
}

EMSCRIPTEN_KEEPALIVE void emu_knob(uint32_t k, int32_t steps) { dub_tower_knob(k & 3u, steps); }

EMSCRIPTEN_KEEPALIVE void emu_note(uint32_t note, int on)
{
    uint32_t st = on ? 0x92u : 0x82u;                   /* channel 3: track 3 */
    if (mi_w - mi_r >= MQ)
        return;
    midi_in_q[mi_w % MQ] = (st >> 4) | st << 8 | (note & 127u) << 16 | (on ? 100u : 0u) << 24;
    RING_PUBLISH();
    mi_w++;
}

EMSCRIPTEN_KEEPALIVE void emu_play(int on) { transport_req = (uint8_t)(on ? 1 : 2); }

EMSCRIPTEN_KEEPALIVE void emu_cc(uint32_t cc, uint32_t v)
{
    uint32_t ch = dub_ctl.midi_ch ? dub_ctl.midi_ch - 1u : 10u;
    if (mi_w - mi_r >= MQ)
        return;
    midi_in_q[mi_w % MQ] = 0x0Bu | (0xB0u | ch) << 8 | (cc & 127u) << 16 | (v & 127u) << 24;
    RING_PUBLISH();
    mi_w++;
}

/* the page's readouts: what is on (pads 1..16 lit, as the FX layer shows them) */
EMSCRIPTEN_KEEPALIVE uint32_t emu_pads_lit(void)
{
    uint32_t m = 0, i;
    for (i = 0; i < 16u; i++)
        if (dub_pad_lit(i))
            m |= 1u << i;
    return m;
}
EMSCRIPTEN_KEEPALIVE int emu_playing(void) { return song.playing; }
EMSCRIPTEN_KEEPALIVE uint32_t emu_band(uint32_t b) { return dub_tel.band_pk[b & 3u]; }   /* (the page's speaker cones) */
