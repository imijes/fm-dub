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
 * dub_ctl.usb_in_q15, dub_usb_audio.c). The processing chain of Phase 3 runs on it here, between the sum and the
 * write-back (dub_chain_run): the saturation (3.3) -> the stepped high-pass (3.2) -> the siren, the echo, the
 * spring (3.3) -> the crossover, kills, meters (3.1) -> the sub bloom, the limiter (3.4).
 * PREAMP BYPASS (menu DUB, pad 12) leaves the chain out; the USB audio is still summed (its own fader).
 *
 * Audio ISR only: no allocation, no lock, no float; reads dub_ctl once per block. Included by felucca.c after
 * usb.c (the USB reader); fx.c calls it through the prototype in dub_shared_state.h. */
#ifdef ENABLE_DUB_PREAMP

typedef struct {
    int32_t l[CTL], r[CTL];           /* Q15 in 32 bits: headroom for the sum and the chain */
    uint32_t n;                       /* frames in this block (<= CTL) */
} dub_sum_bus_t;
static dub_sum_bus_t Stereo_Master_Sum_Bus;

/* BYPASS in and out: the chain's output crossfades with the dry bus over 512 samples (11.6 ms: its phase differs,
 * the crossover's allpasses, and a short fade between two phases of a bass note is a click), and is not run at
 * all once fully bypassed; coming back, its state starts from 0 */
#define DUB_WET_RAMP 64
static struct {
    int32_t w;                        /* wet share, Q15 */
    uint8_t cold;                     /* the chain's state is stale: reset before it runs */
    int32_t dl[CTL], dr[CTL];         /* the dry bus, for the crossfade */
} dub_chain = {0, 1, {0}, {0}};

static void dub_chain_run(dub_sum_bus_t *b)
{
    dub_sat_block(b->l, b->r, b->n);     /* 3.3: the input saturation */
    dub_tubby_block(b->l, b->r, b->n);   /* 3.2: the stepped high-pass, the big knob */
    dub_space_block(b->l, b->r, b->n);   /* 3.3: the siren, the echo, the spring (the throw) */
    dub_xo_block(b->l, b->r, b->n);      /* 3.1: the crossover, the band kills, the band meters */
    dub_sub_block(b->l, b->r, b->n);     /* 3.4: the sub bloom (in on the downbeat) */
    dub_lim_block(b->l, b->r, b->n);     /* 3.4: the safety limiter, the chain's last word */
}

/* fx.c mix_block: mix_l / mix_r in, the preamp's output back in them */
static inline void dub_mix_intercept(int32_t *mix_l, int32_t *mix_r, uint32_t n)
{
    uint32_t i;
    dub_sum_bus_t *b = &Stereo_Master_Sum_Bus;
    if (n > CTL)
        n = CTL;
    b->n = n;
    for (i = 0; i < n; i++) {
        int32_t ul, ur;
#if DUB_UAC_OUT
        dub_usb_audio_read_stereo(&ul, &ur);            /* every frame (its clock loop counts them), fader applied */
#else
        ul = ur = 0;
#endif
        b->l[i] = mix_l[i] + ul;
        b->r[i] = mix_r[i] + ur;
    }
    {
        int32_t to = dub_ctl.bypass ? 0 : 32767;
        if (!to && !dub_chain.w) {                      /* bypassed: the chain sleeps, the meters drop */
            dub_chain.cold = 1;
            for (i = 0; i < DUB_NBAND; i++)
                dub_tel.band_pk[i] = 0;
        } else if (to == 32767 && dub_chain.w == 32767) {
            dub_chain_run(b);
        } else {                                        /* fading in or out */
            if (dub_chain.cold) {
                dub_xo_reset();
                dub_tubby_reset();
                dub_fx_reset();
                dub_sub_reset();
                dub_lim_reset();
                dub_chain.cold = 0;
            }
            for (i = 0; i < n; i++) {
                dub_chain.dl[i] = b->l[i];
                dub_chain.dr[i] = b->r[i];
            }
            dub_chain_run(b);
            for (i = 0; i < n; i++) {
                int32_t w = dub_chain.w;
                w = w < to ? (to - w > DUB_WET_RAMP ? w + DUB_WET_RAMP : to)
                           : (w - to > DUB_WET_RAMP ? w - DUB_WET_RAMP : to);
                dub_chain.w = w;
                b->l[i] = dub_chain.dl[i] + (int32_t)(((int64_t)(b->l[i] - dub_chain.dl[i]) * w) >> 15);
                b->r[i] = dub_chain.dr[i] + (int32_t)(((int64_t)(b->r[i] - dub_chain.dr[i]) * w) >> 15);
            }
        }
    }
    for (i = 0; i < n; i++) {
        mix_l[i] = b->l[i];
        mix_r[i] = b->r[i];
    }
    dub_tel.blocks++;
}

#endif /* ENABLE_DUB_PREAMP */
