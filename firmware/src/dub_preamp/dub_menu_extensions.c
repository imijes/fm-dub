/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 1.2: the DUB section of the SLOOP menu (HOME held) and the preamp settings in flash.
 *
 * MENU, section DUB (between AUDIO and SYSTEM, ui_menu.c):
 *   KNOB 1  MIDI CH    OFF, 1..16 (default 11): the channel the preamp listens to. Amber when it is also a part's
 *                      channel (1..3) or the drum channel (GLO > DRUMS > CH): both would hear it
 *   KNOB 2  PREAMP     ON / BYPASS (the same switch as pad 12 USB BYP on the MIX page)
 *   KNOB 3  SETTINGS   SAVE / DEFAULTS (the knob picks, OCT+ does it): SAVE writes the preamp's settings to flash
 *                      (amber while they differ from it); DEFAULTS puts every preamp setting back as shipped, in
 *                      RAM (SAVE keeps them)
 *   KNOB 4  CONTROL TOWER  OCT+ opens the dashboard (dub_ui_dashboard.c; HOME closes it)
 *
 * Flash: one record in the sector 0xFB000 (free after USR4, fm1_flash.h FL_DUB_LO). The SLOOP settings object is
 * not touched, so the official SLOOP firmware still reads it as its own (going back keeps the SYN kits). One sector,
 * no A/B pair (there is no second free sector): payload first, then the header with both CRCs; a write cut short
 * leaves no valid header and the next boot uses the defaults. Written only when asked (Phase 5): DUB > SETTINGS >
 * SAVE. A knob, a pad, a CC changes the preamp in RAM only; the next power-on comes back to what was saved. The
 * write itself goes with the SLOOP settings (settings_save: when the menu closes, or once stopped and quiet: an
 * erase stops the audio for ~50 ms), so a SAVE while playing is written when the music stops.
 * What is saved: the settings and the sound of the preamp. Not saved: the performance state (kills, siren gate,
 * SUB BLOOM armed, the punch slot, the duck): every power-on starts with nothing killed.
 *
 * Main loop only. Included by felucca.c after project.c (storage.c, flash_ok); ui_menu.c calls through the
 * prototypes in dub_shared_state.h. */
#ifdef ENABLE_DUB_PREAMP

/* ------------------------------------------------------------- the record --- */
#define DUB_REC_MAGIC 0x50425544u     /* "DUBP" */
#define DUB_REC_VER 1u
#define DUB_REC_OFF 0xFB000u          /* FL_DUB_LO */
#define DUB_REC_PAYLOAD 256u          /* payload offset in the sector (header at 0, as storage.c) */

typedef struct {
    uint32_t magic;
    uint16_t ver, len;
    uint32_t crc;                     /* the payload */
    uint32_t hcrc;                    /* the header up to here */
} dub_rec_hdr_t;

typedef struct {                      /* the saved part of dub_ctl_t, plain (not volatile), fixed layout */
    int16_t usb_in_q15, drive_q12, echo_ms, echo_fb_q12;
    int16_t echo_send_q15, echo_ret_q15, rev_send_q15, rev_decay_q15;
    int16_t rev_damp_q15, siren_note_q8, siren_depth_q8, siren_rate_ch;
    int16_t siren_lvl_q15, pan_rate_ch, sub_lvl_q15, out_q15;
    uint8_t bypass, midi_ch, tubby_on, tubby_step;
    uint8_t echo_sync, echo_div, echo_to_rev, siren_wave;
    uint8_t siren_gate_mode, siren_sync, siren_div, pan_on;
    uint8_t pan_div, punch_latch, rsv[2];
} dub_persist_t;
_Static_assert(sizeof(dub_rec_hdr_t) == 16u, "dub record header layout");
_Static_assert(sizeof(dub_persist_t) == 48u, "dub record layout (append new fields, raise DUB_REC_VER)");

static dub_persist_t dub_saved;       /* as in flash (or as last loaded): a save only when it differs */
static uint8_t dub_saved_ok;          /* dub_saved holds a real copy (else: the next save writes) */

/* ranges: what the DSP accepts. A record from another version, or a value out of range, comes back inside */
static int16_t dub_clamp16(int32_t v, int32_t lo, int32_t hi) { return (int16_t)(v < lo ? lo : v > hi ? hi : v); }
static uint8_t dub_clamp8(uint32_t v, uint32_t hi) { return (uint8_t)(v > hi ? hi : v); }

static void dub_persist_fill(dub_persist_t *p)    /* dub_ctl -> the record */
{
    memset(p, 0, sizeof *p);
    p->usb_in_q15 = dub_ctl.usb_in_q15;
    p->drive_q12 = dub_ctl.drive_q12;
    p->echo_ms = dub_ctl.echo_ms;
    p->echo_fb_q12 = dub_ctl.echo_fb_q12;
    p->echo_send_q15 = dub_ctl.echo_send_q15;
    p->echo_ret_q15 = dub_ctl.echo_ret_q15;
    p->rev_send_q15 = dub_ctl.rev_send_q15;
    p->rev_decay_q15 = dub_ctl.rev_decay_q15;
    p->rev_damp_q15 = dub_ctl.rev_damp_q15;
    p->siren_note_q8 = dub_ctl.siren_note_q8;
    p->siren_depth_q8 = dub_ctl.siren_depth_q8;
    p->siren_rate_ch = dub_ctl.siren_rate_ch;
    p->siren_lvl_q15 = dub_ctl.siren_lvl_q15;
    p->pan_rate_ch = dub_ctl.pan_rate_ch;
    p->sub_lvl_q15 = dub_ctl.sub_lvl_q15;
    p->out_q15 = dub_ctl.out_q15;
    p->bypass = dub_ctl.bypass;
    p->midi_ch = dub_ctl.midi_ch;
    p->tubby_on = dub_ctl.tubby_on;
    p->tubby_step = dub_ctl.tubby_step;
    p->echo_sync = dub_ctl.echo_sync;
    p->echo_div = dub_ctl.echo_div;
    p->echo_to_rev = dub_ctl.echo_to_rev;
    p->siren_wave = dub_ctl.siren_wave;
    p->siren_gate_mode = dub_ctl.siren_gate_mode;
    p->siren_sync = dub_ctl.siren_sync;
    p->siren_div = dub_ctl.siren_div;
    p->pan_on = dub_ctl.pan_on;
    p->pan_div = dub_ctl.pan_div;
    p->punch_latch = dub_ctl.punch_latch;
}

static void dub_persist_apply(const dub_persist_t *p)   /* the record -> dub_ctl, each value inside its range */
{
    dub_ctl.usb_in_q15 = dub_clamp16(p->usb_in_q15, 0, 32767);
    dub_ctl.drive_q12 = dub_clamp16(p->drive_q12, DUB_Q12_ONE, DUB_DRIVE_MAX);
    dub_ctl.echo_ms = dub_clamp16(p->echo_ms, 1, DUB_ECHO_MAX_MS);
    dub_ctl.echo_fb_q12 = dub_clamp16(p->echo_fb_q12, 0, DUB_FB_MAX);
    dub_ctl.echo_send_q15 = dub_clamp16(p->echo_send_q15, 0, 32767);
    dub_ctl.echo_ret_q15 = dub_clamp16(p->echo_ret_q15, 0, 32767);
    dub_ctl.rev_send_q15 = dub_clamp16(p->rev_send_q15, 0, 32767);
    dub_ctl.rev_decay_q15 = dub_clamp16(p->rev_decay_q15, 0, 32767);
    dub_ctl.rev_damp_q15 = dub_clamp16(p->rev_damp_q15, 0, 32767);
    dub_ctl.siren_note_q8 = dub_clamp16(p->siren_note_q8, 24 << 8, 108 << 8);   /* C1 .. C8 */
    dub_ctl.siren_depth_q8 = dub_clamp16(p->siren_depth_q8, 0, 24 << 8);
    dub_ctl.siren_rate_ch = dub_clamp16(p->siren_rate_ch, 1, 2000);           /* 0.01 .. 20 Hz */
    dub_ctl.siren_lvl_q15 = dub_clamp16(p->siren_lvl_q15, 0, 32767);
    dub_ctl.pan_rate_ch = dub_clamp16(p->pan_rate_ch, 1, 2000);
    dub_ctl.sub_lvl_q15 = dub_clamp16(p->sub_lvl_q15, 0, 32767);
    dub_ctl.out_q15 = dub_clamp16(p->out_q15, 0, 32767);
    dub_ctl.bypass = dub_clamp8(p->bypass, 1u);
    dub_ctl.midi_ch = dub_clamp8(p->midi_ch, 16u);
    dub_ctl.tubby_on = dub_clamp8(p->tubby_on, 1u);
    dub_ctl.tubby_step = dub_clamp8(p->tubby_step, DUB_TUBBY_NSTEP - 1u);
    dub_ctl.echo_sync = dub_clamp8(p->echo_sync, 1u);
    dub_ctl.echo_div = dub_clamp8(p->echo_div, NDIV_DLY - 1u);
    dub_ctl.echo_to_rev = dub_clamp8(p->echo_to_rev, 1u);
    dub_ctl.siren_wave = dub_clamp8(p->siren_wave, DUB_NWAVE - 1u);
    dub_ctl.siren_gate_mode = dub_clamp8(p->siren_gate_mode, DUB_NGATE - 1u);
    dub_ctl.siren_sync = dub_clamp8(p->siren_sync, 1u);
    dub_ctl.siren_div = dub_clamp8(p->siren_div, NDIV_SHORT - 1u);
    dub_ctl.pan_on = dub_clamp8(p->pan_on, 1u);
    dub_ctl.pan_div = dub_clamp8(p->pan_div, NDIV_SHORT - 1u);
    dub_ctl.punch_latch = dub_clamp8(p->punch_latch, 1u);
    DUB_PUBLISH();
}

static uint8_t dub_save_req;          /* DUB > SETTINGS > SAVE asked, not written yet (project.c settings_save) */
static uint8_t dub_set_pick;          /* the SETTINGS row: 0 SAVE, 1 DEFAULTS */

/* project.c sections_flush: a SAVE is waiting (written once stopped and quiet) */
static int dub_persist_wanted(void) { return dub_save_req; }

/* the settings differ from flash (the SAVE row amber) */
static int dub_persist_changed(void)
{
    dub_persist_t p;
    dub_persist_fill(&p);
    return !dub_saved_ok || memcmp(&p, &dub_saved, sizeof p) != 0;
}

#if FELUCCA_FLASH
static uint8_t dub_rec_buf[sizeof(dub_persist_t)] __attribute__((aligned(4)));   /* RAM: the driver's source */

/* project.c persist_boot, after the SLOOP settings: the defaults, then the record over them */
static void dub_persist_boot(void)
{
    dub_rec_hdr_t h;
    dub_persist_t p;
    dub_ctl_defaults();
    dub_saved_ok = 0;
    if (!flash_ok || st_read(DUB_REC_OFF, &h, sizeof h))
        return;
    if (h.magic != DUB_REC_MAGIC || h.hcrc != st_crc32(&h, sizeof h - 4u) || h.len == 0u || h.len > sizeof p)
        return;                                       /* erased, torn, or not ours: the defaults */
    memset(&p, 0, sizeof p);
    dub_persist_fill(&p);                             /* (an older, shorter record: the rest stays as now) */
    if (st_read(DUB_REC_OFF + DUB_REC_PAYLOAD, dub_rec_buf, h.len) || st_crc32(dub_rec_buf, h.len) != h.crc)
        return;
    memcpy(&p, dub_rec_buf, h.len);
    dub_persist_apply(&p);
    dub_persist_fill(&dub_saved);                     /* (as clamped: a clamped value is written back once) */
    dub_saved_ok = h.len == sizeof p && !memcmp(&dub_saved, &p, sizeof p);
}

/* the record to flash when it differs from it. 0 ok or unchanged, < 0 flash error */
static int dub_persist_write(void)
{
    dub_rec_hdr_t h;
    dub_persist_t p;
    int rc;
    dub_persist_fill(&p);
    if (dub_saved_ok && !memcmp(&p, &dub_saved, sizeof p))
        return 0;                                     /* unchanged: no erase cycle */
    memcpy(dub_rec_buf, &p, sizeof p);
    if ((rc = st_erase(DUB_REC_OFF)) != 0)
        return rc;
    if ((rc = st_prog(DUB_REC_OFF + DUB_REC_PAYLOAD, dub_rec_buf, sizeof dub_rec_buf)) != 0)
        return rc;
    h.magic = DUB_REC_MAGIC;
    h.ver = (uint16_t)DUB_REC_VER;
    h.len = (uint16_t)sizeof p;
    h.crc = st_crc32(dub_rec_buf, sizeof dub_rec_buf);
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if ((rc = st_prog(DUB_REC_OFF, &h, sizeof h)) != 0)   /* the header last: it commits */
        return rc;
    {   /* read back, as storage.c: a failing part must not count as saved */
        dub_rec_hdr_t chk;
        if (st_read(DUB_REC_OFF, &chk, sizeof chk) || memcmp(&chk, &h, sizeof h) ||
            st_read(DUB_REC_OFF + DUB_REC_PAYLOAD, dub_rec_buf, sizeof dub_rec_buf) ||
            st_crc32(dub_rec_buf, sizeof dub_rec_buf) != h.crc)
            return -7;
    }
    dub_saved = p;
    dub_saved_ok = 1;
    return 0;
}

/* project.c settings_save, after a SAVE (dub_save_req): write the record when it differs from flash. 0 ok or
 * unchanged, < 0 flash error (the request stays: tried again at the next quiet save) */
static int dub_persist_save(void)
{
    int rc;
    if (!flash_ok || !dub_save_req)
        return 0;
    rc = dub_persist_write();
    dub_save_req = (uint8_t)(rc != 0);
    return rc;
}
#else
static void dub_persist_boot(void) { dub_ctl_defaults(); }
static int dub_persist_save(void)
{
    dub_save_req = 0;                                 /* (no flash: nothing to write) */
    return 0;
}
#endif

/* -------------------------------------------------------------- the menu --- */
/* the MIDI channel is also a SLOOP channel: parts 1..3, or the drum channel */
static int dub_midi_ch_shared(void)
{
    uint32_t ch = dub_ctl.midi_ch;
    return ch && (ch <= NPART || (song.g[G_DRCH] && ch == (uint32_t)song.g[G_DRCH]));
}

/* ui_menu.c mi_value: the DUB rows' values, and their colour; 0 for an item that is not ours */
static const char *dub_mi_value(uint32_t i, uint16_t *c)
{
    static char b[4];
    *c = C_HI;
    switch (i) {
    case MI_DUBCH:
        if (!dub_ctl.midi_ch)
            return "OFF";
        fmt_int(b, dub_ctl.midi_ch);
        if (dub_midi_ch_shared())
            *c = C_AMB;
        return b;
    case MI_DUBON: return dub_ctl.bypass ? "BYPASS" : "ON";
    case MI_DUBSET:                                   /* SAVE amber while the preamp differs from flash */
        if (dub_set_pick)
            return "DEFAULTS";
        *c = dub_save_req || dub_persist_changed() ? C_AMB : C_DIM;
        return dub_save_req ? "SAVE: WAITING" : "SAVE";
    case MI_DUBTWR:
        *c = C_DIM;
        return "";                                    /* (an action: "OCT+ OPENS") */
    default: return 0;
    }
}

/* ui_menu.c mi_set: a knob turned s, or OCT+ (s = 0); 1 when the item is ours */
static int dub_mi_set(uint32_t i, int32_t s)
{
    switch (i) {
    case MI_DUBCH: {                                  /* right up, left down (stops at OFF and 16); OCT+ steps round */
        int32_t ch = dub_ctl.midi_ch;
        ch = s > 0 ? ch + 1 : s < 0 ? ch - 1 : (ch + 1) % 17;
        dub_ctl.midi_ch = (uint8_t)clamp(ch, 0, 16);
        return 1;
    }
    case MI_DUBON:                                    /* right ON, left BYPASS; OCT+ toggles */
        dub_ctl.bypass = (uint8_t)(s > 0 ? 0u : s < 0 ? 1u : !dub_ctl.bypass);
        return 1;
    case MI_DUBSET:                                   /* the knob: right DEFAULTS, left SAVE; OCT+ does it */
        if (s) {
            dub_set_pick = (uint8_t)(s > 0);
        } else if (dub_set_pick) {
            dub_ctl_defaults();                       /* (RAM: SAVE keeps them) */
            ui_message("DUB DEFAULTS: SAVE KEEPS");
            ui.force = 1;
        } else {
            dub_save_req = 1;                         /* written by settings_save: now when stopped, else once */
            if (!song.playing && !transport_req)      /* stopped and quiet (project.c sections_flush: an erase */
                settings_save();                      /* stops the audio for ~50 ms) */
            ui_message(dub_save_req ? "PREAMP: SAVED WHEN STOPPED" : "PREAMP SAVED");
            ui.force = 1;
        }
        return 1;
    case MI_DUBTWR:
        if (!s) {
            menu_close();                             /* (saves, goes to the TRACKS page) */
            dub_tower_open(ui.page, ui.home);
        }
        return 1;
    default:
        return 0;
    }
}

/* ui_menu.c draw_menu: part of the signature that decides a redraw */
static uint32_t dub_menu_sig(void)
{
    return (uint32_t)dub_ctl.midi_ch * 2654435761u + (uint32_t)dub_ctl.bypass * 40503u +
           (uint32_t)dub_midi_ch_shared() * 9973u + (uint32_t)dub_set_pick * 7907u + (uint32_t)dub_save_req * 6151u +
           (uint32_t)dub_persist_changed() * 4093u;
}

#endif /* ENABLE_DUB_PREAMP */
