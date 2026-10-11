/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 1.1: the shared state between the UI and the audio ISR.
 *
 * The FM-1 runs SLOOP on one pi32v2 core with two contexts:
 *   - the main loop (main.c): keys, encoders, pads, the LCD, the settings in flash
 *   - the ALNK0 audio ISR (audio.c): renders CTL-sample blocks; it also drains the MIDI in queue
 *     (seq.c, events loop), so MIDI is parsed in the ISR, not in the main loop.
 * The ISR preempts the main loop, never the other way round. Every field below has ONE writer:
 *
 *   dub_ctl   main loop -> ISR    the knobs and switches (persisted by Phase 1.2)
 *   dub_live  ISR -> main loop    what MIDI channel DUB_MIDI_CH did (kills, punch, siren gate),
 *                                 so the ISR acts on it in the same block: zero latency
 *   dub_tel   ISR -> main loop    meters and states for the Control Tower page
 *   dub_mq    ISR -> main loop    MIDI CCs on the dub channel, applied to dub_ctl by the main loop
 *                                 (continuous values keep one owner, the one that saves them)
 *
 * Rules (no float, no lock):
 *   - scalars are 8, 16 or 32 bits, naturally aligned: one load or store each, never torn
 *   - only the owner writes a field; read-modify-write (kill bits, toggles) only by the owner
 *   - one-shot actions are counters: the writer increments, the reader acts while seen != count
 *     (an edge is never lost, two quick presses are two actions)
 *   - the ISR reads dub_ctl once per block into its own copy, then smooths toward it (no zipper)
 *   - DUB_PUBLISH() between filling data and moving an index or counter that announces it
 *
 * Number formats: Q15 = 32768 is 1.0 (int16 holds up to 32767); Q12 = 4096 is 1.0, so a value can go
 * past 1.0 (echo feedback for self-oscillation, saturation drive). Times in ms or in N_DLY
 * divisions (core.h dly_units), pitches as MIDI note x 256 (Q8), LFO rates in 1/100 Hz.
 *
 * Built only with ENABLE_DUB_PREAMP. Included once, by felucca.c, after core.h. */
#pragma once
#ifdef ENABLE_DUB_PREAMP
#include <stdint.h>

#ifndef DUB_PUBLISH
#define DUB_PUBLISH() __asm__ volatile("" ::: "memory")   /* (as core.h RING_PUBLISH): stores stay in order */
#endif

/* ------------------------------------------------------------------ constants --- */
#ifdef FS
#define DUB_FS FS
#else
#define DUB_FS 44100                  /* the render rate (core.h FS); USB 48 kHz is resampled in usb.c */
#endif
#define DUB_MIDI_CH_DEF 11u           /* 1..16, 0 = the preamp ignores MIDI */

#define DUB_NBAND 4u                  /* LR4 crossover bands (Phase 3.1) */
enum { DUB_SUB, DUB_BASS, DUB_MID, DUB_TREBLE };
#define DUB_KILL_SUB (1u << DUB_SUB)
#define DUB_KILL_BASS (1u << DUB_BASS)
#define DUB_KILL_MID (1u << DUB_MID)
#define DUB_KILL_TREBLE (1u << DUB_TREBLE)
#define DUB_KILL_ALL 0x0Fu
#define DUB_XO_LO_HZ 80u              /* sub | bass */
#define DUB_XO_MID_HZ 300u            /* bass | mid */
#define DUB_XO_HI_HZ 3500u            /* mid | treble */

#define DUB_TUBBY_NSTEP 11u           /* the stepped high-pass, 70 Hz .. 7.5 kHz (Phase 3.2 tables) */

#define DUB_ECHO_MAX_MS 1200u
#define DUB_ECHO_MAX_N ((uint32_t)DUB_FS * DUB_ECHO_MAX_MS / 1000u)   /* 52920 int16 = 103 KiB of .pool */
#define DUB_Q12_ONE 4096
#define DUB_FB_MAX 5120               /* 1.25: past 1.0 the loop runs away (the limiter catches it) */
#define DUB_DRIVE_MAX 32767           /* Q12: x8 */

enum { DUB_WAVE_SINE, DUB_WAVE_SQUARE, DUB_WAVE_SAW, DUB_NWAVE };
enum { DUB_GATE_MOMENTARY, DUB_GATE_LATCH, DUB_GATE_RETRIG, DUB_NGATE };

#define DUB_NSLOT 16u                 /* the punch-in matrix (Phase 4.2), slot index 0..15 */
enum {
    DUB_PX_KILL_SUB, DUB_PX_KILL_BASS, DUB_PX_KILL_MID, DUB_PX_KILL_TREBLE,
    DUB_PX_ECHO_THROW, DUB_PX_ECHO_FREEZE, DUB_PX_WOW, DUB_PX_STUTTER,
    DUB_PX_SIREN, DUB_PX_SPRING_CRASH, DUB_PX_SPINBACK, DUB_PX_TAPE_STOP,
    DUB_PX_FILTER_SAT, DUB_PX_BLACK_ARK, DUB_PX_RIDDIM_CHOP, DUB_PX_DUCK
};
#define DUB_PX_NONE (-1)

#define DUB_DUCK_Q15 8231             /* -12 dB */

/* the echo's and the spring's memory (dub_effects.c): SLOOP's punch-in ring, 64 KiB of .pool. In this build the
 * punch-in effects do not run (fx.c) and punch.c's ring is this array (they are replaced in Phase 4.2) */
#define DUB_LINE_N 32768u
static int16_t dub_line_mem[DUB_LINE_N] __attribute__((section(".pool")));

/* ------------------------------------------------- main loop -> ISR: settings --- */
typedef struct __attribute__((aligned(4))) {
    /* 16-bit: levels and continuous values */
    volatile int16_t usb_in_q15;      /* the USB fader (PRESETS encoder): the computer's audio into the bus */
    volatile int16_t drive_q12;       /* input saturation: 4096 = x1 .. DUB_DRIVE_MAX */
    volatile int16_t echo_ms;         /* 1 .. DUB_ECHO_MAX_MS when echo_sync = 0 */
    volatile int16_t echo_fb_q12;     /* 0 .. DUB_FB_MAX */
    volatile int16_t echo_send_q15;   /* the bus into the echo */
    volatile int16_t echo_ret_q15;    /* the echo back into the bus */
    volatile int16_t rev_send_q15;    /* the bus into the spring tank */
    volatile int16_t rev_decay_q15;
    volatile int16_t rev_damp_q15;
    volatile int16_t siren_note_q8;   /* base pitch, MIDI note x 256 */
    volatile int16_t siren_depth_q8;  /* LFO depth, semitones x 256 */
    volatile int16_t siren_rate_ch;   /* LFO rate, 1/100 Hz, when siren_sync = 0 */
    volatile int16_t siren_lvl_q15;
    volatile int16_t pan_rate_ch;     /* auto-pan flash rate, 1/100 Hz, when siren_sync = 0 */
    volatile int16_t sub_lvl_q15;     /* the sub-bloom octave-down sine */
    volatile int16_t out_q15;         /* the preamp's output trim, before the limiter */
    /* 8-bit: switches, indices, modes */
    volatile uint8_t bypass;          /* pad 12 USB BYP: the processing chain out (the USB audio still summed) */
    volatile uint8_t midi_ch;         /* 1..16, 0 = off (Phase 1.2 menu) */
    volatile uint8_t kill;            /* DUB_KILL_* from the buttons (OR dub_live.kill) */
    volatile uint8_t tubby_on;
    volatile uint8_t tubby_step;      /* 0 .. DUB_TUBBY_NSTEP-1 */
    volatile uint8_t echo_sync;       /* 1: echo_div instead of echo_ms */
    volatile uint8_t echo_div;        /* N_DLY order (core.h dly_units) */
    volatile uint8_t echo_to_rev;     /* pad 11 ECHO>REV: the echo's feedback line into the tank */
    volatile uint8_t siren_wave;      /* DUB_WAVE_* (the LFO shape) */
    volatile uint8_t siren_gate_mode; /* DUB_GATE_* */
    volatile uint8_t siren_gate;      /* pad / button held or latched */
    volatile uint8_t siren_sync;      /* pad 13 SIREN CLK: LFO and pan follow the tempo */
    volatile uint8_t siren_div;       /* N_DIV order when siren_sync */
    volatile uint8_t pan_on;          /* ping-pong auto-pan of the siren */
    volatile uint8_t pan_div;         /* N_DIV order when siren_sync */
    volatile uint8_t sub_arm;         /* pad 14 SUB BLOOM: wanted on/off; the ISR switches it at step 0 */
    volatile int8_t punch;            /* DUB_PX_* held or latched from the keys, DUB_PX_NONE */
    volatile uint8_t punch_latch;     /* 1: the punch stays on after the key is released */
    volatile uint8_t duck;            /* SELECTOR'S DUCK held */
    volatile uint8_t pad0;
    /* 32-bit: one-shot counters (the ISR keeps its own "seen" copy) */
    volatile uint32_t ev_crash;       /* spring tank crash */
    volatile uint32_t ev_siren;       /* siren re-trigger */
} dub_ctl_t;

/* ---------------------------------------------- ISR -> main loop: MIDI layer --- */
/* Written by the ISR from the dub MIDI channel; the DSP ORs it with dub_ctl (kills, gate) or lets it
 * win (punch). The UI reads it to light the pads. */
typedef struct __attribute__((aligned(4))) {
    volatile uint8_t kill;            /* DUB_KILL_*, notes or CC toggles */
    volatile int8_t punch;            /* DUB_PX_*, DUB_PX_NONE */
    volatile uint8_t siren_gate;
    volatile uint8_t duck;
    volatile uint32_t ev_crash;       /* (as dub_ctl: counters) */
    volatile uint32_t ev_siren;
} dub_live_t;

/* ---------------------------------------------- ISR -> main loop: telemetry --- */
typedef struct __attribute__((aligned(4))) {
    volatile uint32_t blocks;         /* blocks rendered by the preamp (the UI sees it move) */
    volatile uint32_t lim_hits;       /* blocks where the limiter reduced the gain */
    volatile uint16_t band_pk[DUB_NBAND];   /* Q15 peak with release, after the kills: the VU pillars */
    volatile uint16_t echo_env;       /* Q15 level of the echo loop: the scope ring's radius */
    volatile uint16_t lim_gr_q15;     /* limiter gain now, 32767 = none */
    volatile uint16_t siren_ph;       /* LFO phase, 65536 = one cycle: the wave preview */
    volatile int16_t siren_pan;       /* -32767 left .. 32767 right */
    volatile uint8_t sub_on;          /* the sub-bloom is sounding (it waited for step 0) */
    volatile uint8_t tubby_step;      /* the step the filter is on (it follows dub_ctl at a block boundary) */
    volatile uint8_t punch;           /* the slot playing, 0xFF none */
    volatile uint8_t usb_in_live;     /* the computer is sending audio */
} dub_tel_t;

/* ------------------------------------------- ISR -> main loop: MIDI CC queue --- */
#define DUB_MQ 32u                    /* power of two */
typedef struct {
    volatile uint16_t q[DUB_MQ];      /* cc << 8 | value */
    volatile uint32_t w;              /* the ISR's */
    volatile uint32_t r;              /* the main loop's */
} dub_mq_t;

_Static_assert(sizeof(dub_ctl_t) == 60u, "dub_ctl_t layout");
_Static_assert(sizeof(dub_live_t) == 12u, "dub_live_t layout");
_Static_assert(sizeof(dub_tel_t) == 28u, "dub_tel_t layout");
_Static_assert((DUB_MQ & (DUB_MQ - 1u)) == 0, "DUB_MQ is a power of two");

static dub_ctl_t dub_ctl;
static dub_live_t dub_live = {.punch = DUB_PX_NONE};
static dub_tel_t dub_tel = {.lim_gr_q15 = 32767, .punch = 0xFFu};
static dub_mq_t dub_mq;

/* ------------------------------------------------------------------ helpers --- */
static void dub_ctl_defaults(void)    /* main loop, at boot, before the settings load over it */
{
    dub_ctl.usb_in_q15 = 32767;
    dub_ctl.drive_q12 = DUB_Q12_ONE;
    dub_ctl.echo_ms = 375;
    dub_ctl.echo_fb_q12 = 2458;       /* 0.6 */
    dub_ctl.echo_send_q15 = 0;
    dub_ctl.echo_ret_q15 = 16384;
    dub_ctl.rev_send_q15 = 0;
    dub_ctl.rev_decay_q15 = 22938;    /* 0.7 */
    dub_ctl.rev_damp_q15 = 16384;
    dub_ctl.siren_note_q8 = 74 << 8;  /* D5, 587 Hz */
    dub_ctl.siren_depth_q8 = 12 << 8;
    dub_ctl.siren_rate_ch = 400;      /* 4 Hz */
    dub_ctl.siren_lvl_q15 = 16384;
    dub_ctl.pan_rate_ch = 200;
    dub_ctl.sub_lvl_q15 = 16384;
    dub_ctl.out_q15 = 32767;
    dub_ctl.bypass = 0;
    dub_ctl.midi_ch = DUB_MIDI_CH_DEF;
    dub_ctl.kill = 0;
    dub_ctl.tubby_on = 0;
    dub_ctl.tubby_step = 0;
    dub_ctl.echo_sync = 1;
    dub_ctl.echo_div = 6;             /* 1/8 dotted (N_DLY: 1/4 1/8 1/16 1/32 8T 16T 1/8D 1/16D) */
    dub_ctl.echo_to_rev = 0;
    dub_ctl.siren_wave = DUB_WAVE_SINE;
    dub_ctl.siren_gate_mode = DUB_GATE_MOMENTARY;
    dub_ctl.siren_gate = 0;
    dub_ctl.siren_sync = 0;
    dub_ctl.siren_div = 1;            /* 1/8 (N_DIV: 1/4 1/8 1/16 1/32 8T 16T) */
    dub_ctl.pan_on = 0;
    dub_ctl.pan_div = 0;              /* 1/4 */
    dub_ctl.sub_arm = 0;
    dub_ctl.punch = DUB_PX_NONE;
    dub_ctl.punch_latch = 0;
    dub_ctl.duck = 0;
    dub_ctl.pad0 = 0;
    DUB_PUBLISH();
}

/* one-shot counters: the writer bumps, the reader catches up (one action per call) */
static inline void dub_ev_post(volatile uint32_t *ev) { *ev = *ev + 1u; }
static inline int dub_ev_take(volatile const uint32_t *ev, uint32_t *seen)
{
    if (*seen == *ev)
        return 0;
    (*seen)++;
    return 1;
}

/* the CC queue: dub_mq_put in the ISR, dub_mq_get in the main loop. A full queue drops the CC. */
static inline void dub_mq_put(uint32_t cc, uint32_t val)
{
    uint32_t w = dub_mq.w;
    if (w - dub_mq.r >= DUB_MQ)
        return;
    dub_mq.q[w & (DUB_MQ - 1u)] = (uint16_t)((cc & 127u) << 8 | (val & 127u));
    DUB_PUBLISH();
    dub_mq.w = w + 1u;
}
static inline int dub_mq_get(uint32_t *cc, uint32_t *val)
{
    uint32_t r = dub_mq.r, e;
    if (r == dub_mq.w)
        return 0;
    DUB_PUBLISH();
    e = dub_mq.q[r & (DUB_MQ - 1u)];
    *cc = e >> 8;
    *val = e & 127u;
    DUB_PUBLISH();
    dub_mq.r = r + 1u;
    return 1;
}

/* what the DSP plays: the buttons and MIDI together */
static inline uint32_t dub_kill_now(void) { return (uint32_t)(dub_ctl.kill | dub_live.kill) & DUB_KILL_ALL; }
static inline int32_t dub_punch_now(void) { return dub_live.punch != DUB_PX_NONE ? dub_live.punch : dub_ctl.punch; }

/* ---------------------------------------- dub_menu_extensions.c (Phase 1.2) --- */
/* defined after project.c (they need storage.c); called by ui_menu.c and project.c. (inline here: a build or a test
 * that leaves the module out does not warn about them) */
static inline const char *dub_mi_value(uint32_t i, uint16_t *c);
static inline int dub_mi_set(uint32_t i, int32_t s);
static inline uint32_t dub_menu_sig(void);
static inline void dub_persist_boot(void);
static inline int dub_persist_save(void);
static inline int dub_persist_changed(void);
/* dub_mixer_intercept.c (Phase 2.2): defined after usb.c (the USB reader); called by fx.c mix_block */
static inline void dub_mix_intercept(int32_t *mix_l, int32_t *mix_r, uint32_t n);
/* dub_midi.c: the preamp's MIDI channel; called by seq.c (audio ISR) and main.c (main loop) */
static int dub_midi_take(uint32_t pkt);
static void dub_mq_apply(void);

#endif /* ENABLE_DUB_PREAMP */
