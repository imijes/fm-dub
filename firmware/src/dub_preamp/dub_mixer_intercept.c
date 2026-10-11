/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 2.2: the mixer intercept. Where the preamp takes the whole SLOOP mix.
 *
 * The ALNK0 audio ISR (audio.c fm1_alnk0_irq) renders each half buffer in blocks of CTL frames: audio_block ->
 * mix_block (fx.c): events -> the three synth parts -> the drum track -> the FX buses -> DUST -> PUNCH -> the DJ
 * filter -> [here] -> MASTER volume -> limiter, soft knee (master_out) -> 24 bit. fx.c calls dub_mix_intercept
 * right after the DJ filter, on mix_l / mix_r: the four tracks with their effects, summed, Q15 (32767 = the
 * master's full scale, before the MASTER knob). So the knob, SLOOP's limiter and the soft knee stay after the
 * preamp and protect the output whatever the preamp does; the visualiser and the USB recording (MASTER or FULL)
 * hear the preamp too.
 *
 * Stereo_Master_Sum_Bus: 32-bit stereo, one block: the SLOOP mix + the computer's audio (USB playback, its fader
 * dub_ctl.usb_in_q15, dub_usb_audio.c; the Mix grid's MUTE 5 / SOLO 5 as SLOOP's mute and solo: a mute wins, a solo
 * of the USB alone silences the SLOOP mix and its sends, a SLOOP solo without SOLO 5 silences the USB). Then:
 *   [colour]  the saturation (3.3) -> the stepped high-pass (3.2)
 *   always    the siren, the echo, the spring (3.3, Phase 5: SLOOP's delay and reverb, fed by its sends send_d /
 *             send_r too) -> their return and the siren to the post buffer; the THROW dry fade on the bus
 *   [colour]  the crossover, kills, meters (3.1) -> the sub bloom (3.4)
 *   always    + the post buffer (no saturation, no kill on the echoes and the siren) -> the limiter (3.4)
 * PREAMP BYPASS (menu DUB, pad 12) takes out the colour, crossfaded over 11.6 ms; SLOOP's delay and reverb (now the
 * tape echo and the spring) and the limiter stay: BYPASS is the mix, 8 samples late (the limiter's look-ahead).
 *
 * Audio ISR only: no allocation, no lock, no float; reads dub_ctl once per block. Included by felucca.c after
 * usb.c (the USB reader); fx.c calls it through the prototype in dub_shared_state.h. */
#ifdef ENABLE_DUB_PREAMP

typedef struct {
    int32_t l[CTL], r[CTL];           /* Q15 in 32 bits: headroom for the sum and the chain */
    uint32_t n;                       /* frames in this block (<= CTL) */
} dub_sum_bus_t;
static dub_sum_bus_t Stereo_Master_Sum_Bus;

/* BYPASS in and out: the colour's output crossfades with the dry bus over 512 samples (11.6 ms: its phase differs,
 * the crossover's allpasses, and a short fade between two phases of a bass note is a click), and is not run at
 * all once fully bypassed; coming back, its state starts from 0 */
#define DUB_WET_RAMP 64
#define DUB_GAIN_RAMP 1024            /* MUTE 5 / SOLO 5: 32 samples */
static struct {
    int32_t w;                        /* the colour's share, Q15 */
    uint8_t cold;                     /* the colour's state is stale: reset before it runs */
    int32_t gs, gu;                   /* the SLOOP mix's and the USB audio's gain (mute / solo), Q15 */
    int32_t dl[CTL], dr[CTL];         /* the dry bus, for the crossfade */
    int32_t pl[CTL], pr[CTL];         /* the post buffer: the siren, the echo and spring's return */
    int32_t sd[CTL], sr[CTL];         /* SLOOP's delay and reverb sends, at the SLOOP mix's gain */
} dub_chain = {.cold = 1, .gs = 32767, .gu = 32767};

static inline int32_t dub_ramp(int32_t g, int32_t to)
{
    return g < to ? (to - g > DUB_GAIN_RAMP ? g + DUB_GAIN_RAMP : to) : (g - to > DUB_GAIN_RAMP ? g - DUB_GAIN_RAMP : to);
}

/* fx.c mix_block: mix_l / mix_r in (SLOOP's mix after its FX), send_d / send_r (its delay and reverb sends, for the
 * tape echo and the spring); the preamp's output back in mix_l / mix_r */
static inline void dub_mix_intercept(int32_t *mix_l, int32_t *mix_r, const int32_t *send_d, const int32_t *send_r,
                                     uint32_t n)
{
    uint32_t i;
    dub_sum_bus_t *b = &Stereo_Master_Sum_Bus;
    int32_t to = dub_ctl.bypass ? 0 : 32767, ts, tu;
    int colour, fading;
    if (n > CTL)
        n = CTL;
    b->n = n;
    ts = dub_ctl.usb_solo && !song.solo ? 0 : 32767;   /* (the SLOOP mix: a solo of the USB alone) */
    tu = dub_ctl.usb_mute || (song.solo && !dub_ctl.usb_solo) ? 0 : 32767;
    for (i = 0; i < n; i++) {
        int32_t ul, ur, gs, gu;
#if DUB_UAC_OUT
        dub_usb_audio_read_stereo(&ul, &ur);            /* every frame (its clock loop counts them), fader applied */
#else
        ul = ur = 0;
#endif
        gs = dub_chain.gs = dub_ramp(dub_chain.gs, ts);
        gu = dub_chain.gu = dub_ramp(dub_chain.gu, tu);
        if (gu != 32767) {                              /* (32767 is unity: the samples untouched, bit for bit) */
            ul = (int32_t)(((int64_t)ul * gu) >> 15);
            ur = (int32_t)(((int64_t)ur * gu) >> 15);
        }
        if (gs == 32767) {
            b->l[i] = mix_l[i] + ul;
            b->r[i] = mix_r[i] + ur;
            dub_chain.sd[i] = send_d[i];
            dub_chain.sr[i] = send_r[i];
        } else {
            b->l[i] = (int32_t)(((int64_t)mix_l[i] * gs) >> 15) + ul;
            b->r[i] = (int32_t)(((int64_t)mix_r[i] * gs) >> 15) + ur;
            dub_chain.sd[i] = (int32_t)(((int64_t)send_d[i] * gs) >> 15);
            dub_chain.sr[i] = (int32_t)(((int64_t)send_r[i] * gs) >> 15);
        }
    }
    colour = to || dub_chain.w;
    fading = colour && !(to == 32767 && dub_chain.w == 32767);
    if (colour) {
        if (dub_chain.cold) {
            dub_xo_reset();
            dub_tubby_reset();
            dub_sub_reset();
            dub_chain.cold = 0;
        }
        if (fading)
            for (i = 0; i < n; i++) {
                dub_chain.dl[i] = b->l[i];
                dub_chain.dr[i] = b->r[i];
            }
        dub_sat_block(b->l, b->r, n);                   /* 3.3: the input saturation */
        dub_tubby_block(b->l, b->r, n);                 /* 3.2: the stepped high-pass, the big knob */
    } else {
        dub_chain.cold = 1;                             /* bypassed: the colour sleeps, the meters drop */
        for (i = 0; i < DUB_NBAND; i++)
            dub_tel.band_pk[i] = 0;
    }
    dub_space_block(b->l, b->r, fading ? dub_chain.dl : 0, fading ? dub_chain.dr : 0, dub_chain.sd, dub_chain.sr,
                    dub_chain.pl, dub_chain.pr, n);   /* 3.3: the siren, the echo, the spring (the throw) */
    if (colour) {
        dub_xo_block(b->l, b->r, n);                    /* 3.1: the crossover, the band kills, the band meters */
        dub_sub_block(b->l, b->r, n);                   /* 3.4: the sub bloom (in on the downbeat) */
        if (fading)
            for (i = 0; i < n; i++) {
                int32_t w = dub_chain.w;
                w = w < to ? (to - w > DUB_WET_RAMP ? w + DUB_WET_RAMP : to)
                           : (w - to > DUB_WET_RAMP ? w - DUB_WET_RAMP : to);
                dub_chain.w = w;
                b->l[i] = dub_chain.dl[i] + (int32_t)(((int64_t)(b->l[i] - dub_chain.dl[i]) * w) >> 15);
                b->r[i] = dub_chain.dr[i] + (int32_t)(((int64_t)(b->r[i] - dub_chain.dr[i]) * w) >> 15);
            }
    }
    for (i = 0; i < n; i++) {                           /* the echoes and the siren: after the colour */
        b->l[i] += dub_chain.pl[i];
        b->r[i] += dub_chain.pr[i];
    }
    dub_lim_block(b->l, b->r, n);                       /* 3.4: the safety limiter, always the last word */
    for (i = 0; i < n; i++) {
        mix_l[i] = b->l[i];
        mix_r[i] = b->r[i];
    }
    dub_tel.blocks++;
}

#endif /* ENABLE_DUB_PREAMP */
