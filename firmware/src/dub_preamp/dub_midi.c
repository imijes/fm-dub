/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP: the preamp's MIDI channel (dub_ctl.midi_ch, menu DUB > MIDI CH, default 11).
 *
 * Every message on that channel belongs to the preamp: SLOOP's tracks never see it (seq.c hands it here before its
 * own routing), whatever the channel is. Notes and anything but CCs on it are dropped. The CCs:
 *
 *   in the audio ISR (dub_midi_take, the same block: no latency), into dub_live:
 *     30..33  SUB, BASS, MID, TREBLE kill: value >= 64 killed, < 64 open
 *     40..44  punch slots 5..9: ECHO THROW, ECHO FREEZE, WOW & FLUTTER, STUTTER ROLL (Phase 4.2), SIREN: >= 64 on,
 *             < 64 off (the slot that was on)
 *     45      SPRING CRASH (slot 10): >= 64 fires one crash
 *   in the main loop (dub_mq_apply, through dub_mq: the settings keep one owner, the one that saves them):
 *     7   USB fader (dub_ctl.usb_in_q15), squared: a fader's taper
 *     20  the big knob (tubby_step 0..10; turns the filter in)    21  drive (drive_q12 x1 .. x8)
 *     22  echo feedback (echo_fb_q12 0 .. 1.25)                    23  siren pitch (siren_note_q8 C1 .. C8)
 *     50  ECHO>REV   51  PREAMP BYPASS   52  SIREN CLK (sync)   53  SUB BLOOM arm: >= 64 on, < 64 off
 * Switches are set by the value, not toggled by each message: a controller's button sends 127 then 0 (or, latching,
 * 127 on the first press and 0 on the second), and either way the switch follows the button. CCs 120..127 (MIDI's
 * channel mode messages) are dropped: 120..123 are the pads' toggles inside the queue.
 *
 * THE PADS (Phase 4.2): FX held, the 16 white keys (F3 .. G5, punch.c punch_key: pads 1..16) are the preamp's, in
 * the audio ISR (seq.c key_down / key_up), as the MIDI CCs:
 *   1..4   SUB, BASS, MID, TREBLE kill while held          5..9   ECHO THROW, ECHO FREEZE, WOW & FLUTTER, STUTTER
 *   10     SPRING CRASH (one, on the press)                       ROLL, SIREN: the punch slot while held
 *   11..14 ECHO>REV, BYPASS, SIREN CLK, SUB BLOOM: toggled on the press (through the queue: dub_ctl is the main
 *          loop's)                                         15, 16 nothing
 * The kills of the pads and of MIDI are kept apart and ORed into dub_live.kill: a pad let go does not open a band
 * that MIDI holds killed (nor the other way). */
#ifdef ENABLE_DUB_PREAMP

#define DUB_CC_TOGGLE 120u            /* the queue's toggles: 120 ECHO>REV, 121 BYPASS, 122 SIREN CLK, 123 SUB BLOOM */

static struct {                       /* the audio ISR's */
    uint8_t midi_kill, pad_kill;      /* the bands each holds killed */
    uint32_t kill_key[DUB_NBAND];     /* the key holding each pad kill */
    uint32_t punch_key;               /* the key holding the pads' punch slot */
} dub_pad;

static void dub_kill_publish(void) { dub_live.kill = (uint8_t)(dub_pad.midi_kill | dub_pad.pad_kill); }

/* seq.c key_down, FX held (the audio ISR): pad i (0..15), keybit its key */
static void dub_pad_down(uint32_t i, uint32_t keybit)
{
    if (i < DUB_NBAND) {
        dub_pad.pad_kill |= (uint8_t)(1u << i);
        dub_pad.kill_key[i] = keybit;
        dub_kill_publish();
    } else if (i < 10u) {
        int8_t slot = (int8_t)(DUB_PX_ECHO_THROW + (int32_t)(i - 4u));
        if (slot == DUB_PX_SPRING_CRASH) {
            dub_ev_post(&dub_live.ev_crash);
        } else {
            dub_live.punch = slot;
            dub_pad.punch_key = keybit;
        }
    } else if (i < 14u) {
        dub_mq_put(DUB_CC_TOGGLE + (i - 10u), 127u);
    }
}

/* seq.c key_up of a key that went down with FX held */
static void dub_pad_up(uint32_t keybit)
{
    uint32_t b;
    for (b = 0; b < DUB_NBAND; b++)
        if (dub_pad.kill_key[b] == keybit) {
            dub_pad.kill_key[b] = 0;
            dub_pad.pad_kill &= (uint8_t)~(1u << b);
            dub_kill_publish();
        }
    if (dub_pad.punch_key == keybit) {
        dub_pad.punch_key = 0;
        dub_live.punch = DUB_PX_NONE;
    }
}

/* seq.c, the audio ISR's MIDI loop: 1 when the packet was the preamp's (taken), 0 for SLOOP */
static int dub_midi_take(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 0x0Fu, cc = (pkt >> 16) & 0x7Fu, v = (pkt >> 24) & 0x7Fu;
    if (!dub_ctl.midi_ch || ch + 1u != dub_ctl.midi_ch)
        return 0;
    if (st != 0xB0u)
        return 1;                                       /* the preamp's channel: nothing of it reaches the tracks */
    if (cc >= 30u && cc <= 33u) {
        uint8_t bit = (uint8_t)(1u << (cc - 30u));
        dub_pad.midi_kill = (uint8_t)(v >= 64u ? dub_pad.midi_kill | bit : dub_pad.midi_kill & ~bit);
        dub_kill_publish();
    } else if (cc >= 40u && cc <= 44u) {
        int8_t slot = (int8_t)(DUB_PX_ECHO_THROW + (int32_t)(cc - 40u));
        if (v >= 64u)
            dub_live.punch = slot;
        else if (dub_live.punch == slot)
            dub_live.punch = DUB_PX_NONE;
    } else if (cc == 45u) {
        if (v >= 64u)
            dub_ev_post(&dub_live.ev_crash);
    } else if (cc < DUB_CC_TOGGLE) {
        dub_mq_put(cc, v);                              /* (120..127: channel mode messages, not the preamp's) */
    }
    return 1;
}

/* main.c, the main loop, every pass: the CCs for dub_ctl */
static void dub_mq_apply(void)
{
    uint32_t cc, v;
    while (dub_mq_get(&cc, &v)) {
        switch (cc) {
        case 7: dub_ctl.usb_in_q15 = (int16_t)(v * v * 32767u / (127u * 127u)); break;
        case 20:
            dub_ctl.tubby_step = (uint8_t)(v * DUB_TUBBY_NSTEP >> 7);   /* 0..127 -> 0..10, even steps */
            dub_ctl.tubby_on = 1;
            break;
        case 21: dub_ctl.drive_q12 = (int16_t)(DUB_Q12_ONE + v * (uint32_t)(DUB_DRIVE_MAX - DUB_Q12_ONE) / 127u); break;
        case 22: dub_ctl.echo_fb_q12 = (int16_t)(v * (uint32_t)DUB_FB_MAX / 127u); break;
        case 23: dub_ctl.siren_note_q8 = (int16_t)((24u << 8) + v * (84u << 8) / 127u); break;
        case 50: dub_ctl.echo_to_rev = v >= 64u; break;
        case 51: dub_ctl.bypass = v >= 64u; break;
        case 52: dub_ctl.siren_sync = v >= 64u; break;
        case 53: dub_ctl.sub_arm = v >= 64u; break;
        case DUB_CC_TOGGLE: dub_ctl.echo_to_rev = !dub_ctl.echo_to_rev; break;   /* the pads 11..14 */
        case DUB_CC_TOGGLE + 1u: dub_ctl.bypass = !dub_ctl.bypass; break;
        case DUB_CC_TOGGLE + 2u: dub_ctl.siren_sync = !dub_ctl.siren_sync; break;
        case DUB_CC_TOGGLE + 3u: dub_ctl.sub_arm = !dub_ctl.sub_arm; break;
        default: break;
        }
    }
}

#endif /* ENABLE_DUB_PREAMP */
