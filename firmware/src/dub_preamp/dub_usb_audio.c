/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 2.1: the computer's audio into the FM-1 (USB playback, UAC1, dub_usb_audio.h).
 *
 *   TIMER5 ISR (usb_poll, 2 kHz, nested in the render too):  dub_uac_service takes each EP4 OUT packet (one per
 *        1 ms frame, 44 / 45 frames) into dub_ua_ring: the producer, owns dub_ua_w
 *   audio ISR:  dub_usb_audio_read_stereo, once per output frame: the consumer, owns dub_ua_r and dub_rd
 *
 * Clocks: the host sends 44,100 frames a second by its clock, the I2S takes ~44,117.6 by the FM-1's (adaptive
 * endpoint: the device follows the host). The reader resamples by a ratio near 1 (linear interpolation, Q16
 * step) that a PI loop sets from the ring's fill, measured every DUB_UA_CTL frames read (the same point of the
 * render each time: the reader must be called for every frame, the gain at 0 too). Target fill: a half buffer
 * and 2.9 ms more (USB jitter, a late poll). The step stays within +-0.5 %; in steady state it sits ~0.04 % slow.
 * Start: silence until the ring holds the target, then a fade in. Underrun (the host stopped, a flash erase held
 * the interrupts): a fade out of the last frame and a new start. No division, no float. */
#if DUB_UAC_OUT

/* ring, frames (L | R << 16, 16 bit each); a power of two. 4 KiB of RAM (.bss), not .pool: the pool has 10 KiB
 * left and the build keeps 8 KiB of it spare (tools/build.py check). The fill peaks near 800 (tests/dub_usb_test.c) */
#define DUB_UA_N 1024u
#define DUB_UA_TARGET (HALF_FRAMES + 128u)
#define DUB_UA_CTL 256u               /* frames between two fill measurements (a power of two) */
#define DUB_UA_STEP1 65536u           /* Q16: one frame */
#define DUB_UA_TRIM 328               /* +-0.5 % of a frame, Q16 */
#define DUB_UA_IDLE 40u               /* polls (20 ms) without a packet: the computer is not playing */
_Static_assert((DUB_UA_N & (DUB_UA_N - 1u)) == 0 && DUB_UA_TARGET * 2u + DUB_UA_MAXF < DUB_UA_N,
               "the ring holds twice the target and a packet");

static uint32_t dub_ua_ring[DUB_UA_N];
static volatile uint32_t dub_ua_w, dub_ua_r;
static uint8_t dub_ep4rx[DUB_UA_MAXP + 4u] __attribute__((aligned(4)));   /* EP4 OUT DMA (+4 as ep1rx) */

static struct {                       /* the producer's (TIMER5: usb_poll, ep0_service) */
    volatile uint8_t alt;             /* IF 3 setting: 1 = the host may play */
    volatile uint8_t gen;             /* bumped at every stream (re)start: the reader starts over */
    volatile uint8_t live;            /* packets are arriving */
    uint8_t idle;                     /* polls since the last packet */
    uint32_t pkts, frames;
    uint32_t overruns;                /* frames dropped: the ring was full (the reader stopped) */
    uint32_t odd;                     /* packets with a length not a whole number of frames, or too long */
} dub_uac;

static struct {                       /* the reader's (audio ISR) */
    uint8_t gen, go;
    uint32_t frac;                    /* Q16 position between the frame at dub_ua_r and the next */
    uint32_t step;                    /* Q16 frames per output frame */
    int32_t fill_q4;                  /* smoothed fill, Q4 */
    int32_t integ_q8;                 /* the PI loop's integral, Q8 of a Q16 step */
    uint32_t n;                       /* frames read (the control interval) */
    int32_t g;                        /* gain Q15, ramped to dub_ctl.usb_in_q15 (0 while stopped) */
    int32_t last_l, last_r;           /* the last frame: held while fading out */
    uint32_t starts, underruns;
} dub_rd = {.step = DUB_UA_STEP1};

/* ----------------------------------------------------- producer: TIMER5 --- */
/* SET_INTERFACE (IF 3), SET_CONFIGURATION, bus reset: EP4 OUT back to isochronous, empty */
static void dub_uac_stream(uint32_t alt)
{
    if (usb.config) {
        sie_wr(S_INDEX, 4);
        sie_wr(S_RXCSR1, 0x90);                         /* ClrDataTog + FlushFIFO (as ep1_config) */
        sie_wr(S_RXCSR2, 0x40);                         /* ISO */
    }
    dub_uac.live = 0;
    dub_uac.idle = 0;
    RING_PUBLISH();
    dub_uac.alt = (uint8_t)alt;
    dub_uac.gen++;                                      /* the reader drops what the ring holds */
}

static void dub_uac_ep_config(void)                    /* ep1_config, SET_CONFIGURATION 1 */
{
    fm1_usb_ep4_rxbuf(dub_ep4rx);
    sie_wr(S_INDEX, 4);
    sie_wr(S_RXMAXP, 0xFF);                             /* (as EP1 / EP3 and EP4 IN) */
    dub_uac_stream(0);
}

/* one packet into the ring; usb_poll, every poll while alt 1 (the RX flag of EP4 is read here, not in
 * INTRRX1: one SIE read, as uac_service reads TXCSR) */
static void dub_uac_service(void)
{
    uint32_t csr, n, i, w, room;
    const uint8_t *b = dub_ep4rx;
    if (!usb.up || !usb.config || !dub_uac.alt)
        return;
    sie_wr(S_INDEX, 4);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {                                  /* no packet */
        if (dub_uac.idle < 255u && ++dub_uac.idle == DUB_UA_IDLE)
            dub_uac.live = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > DUB_UA_MAXP || (n & 3u)) {
        dub_uac.odd++;
        if (n > DUB_UA_MAXP)
            n = DUB_UA_MAXP;
    }
    n >>= 2;
    fm1_usb_rx_sync();                                  /* the DMA's bytes before we read them */
    w = dub_ua_w;
    room = DUB_UA_N - (w - dub_ua_r);
    if (n > room) {
        dub_uac.overruns += n - room;
        n = room;
    }
    for (i = 0; i < n; i++, b += 4)                     /* little-endian L, R */
        dub_ua_ring[(w + i) & (DUB_UA_N - 1u)] = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                                                 (uint32_t)b[3] << 24;
    RING_PUBLISH();                                     /* the slots before the index */
    dub_ua_w = w + n;
    dub_uac.pkts++;
    dub_uac.frames += n;
    dub_uac.idle = 0;
    dub_uac.live = 1;
    csr = (csr & ~0x164u) | 0x10u;                      /* the packet is taken (as ep1_rx) */
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

/* ------------------------------------------------- consumer: audio ISR --- */
static inline int32_t dub_ua_l(uint32_t f) { return (int32_t)(int16_t)(f & 0xFFFFu); }
static inline int32_t dub_ua_r16(uint32_t f) { return (int32_t)(int16_t)(f >> 16); }

/* the PI loop: fill above the target reads faster, below slower */
static void dub_ua_control(uint32_t fill)
{
    int32_t err, trim;
    dub_rd.fill_q4 += ((int32_t)(fill << 4) - dub_rd.fill_q4) >> 3;
    err = (dub_rd.fill_q4 >> 4) - (int32_t)DUB_UA_TARGET;
    dub_rd.integ_q8 += err;
    if (dub_rd.integ_q8 > DUB_UA_TRIM << 8)
        dub_rd.integ_q8 = DUB_UA_TRIM << 8;
    else if (dub_rd.integ_q8 < -(DUB_UA_TRIM << 8))
        dub_rd.integ_q8 = -(DUB_UA_TRIM << 8);
    trim = (err >> 1) + (dub_rd.integ_q8 >> 8);
    if (trim > DUB_UA_TRIM)
        trim = DUB_UA_TRIM;
    else if (trim < -DUB_UA_TRIM)
        trim = -DUB_UA_TRIM;
    dub_rd.step = (uint32_t)((int32_t)DUB_UA_STEP1 + trim);
}

/* One output frame of the computer's audio, Q15 (the scale of mix_block: 32767 = the master's full scale),
 * with the USB fader (dub_ctl.usb_in_q15) applied. Audio ISR, once per frame, always (silence: 0, 0). */
static inline void dub_usb_audio_read_stereo(int32_t *left, int32_t *right)
{
    uint32_t w = dub_ua_w, r = dub_ua_r, fill = w - r, adv;
    int32_t gt = dub_ctl.usb_in_q15, l, rr;
    RING_PUBLISH();                                     /* the index before the slots */
    if (dub_rd.gen != dub_uac.gen) {                    /* a new stream: start over, empty */
        dub_rd.gen = dub_uac.gen;
        dub_rd.go = 0;
        dub_ua_r = r = w;
        fill = 0;
    }
    if (!dub_rd.go) {
        if (dub_uac.alt && dub_uac.live && fill >= DUB_UA_TARGET) {
            dub_rd.go = 1;                              /* primed: fade in */
            dub_rd.frac = 0;
            dub_rd.step = DUB_UA_STEP1;
            dub_rd.fill_q4 = (int32_t)(fill << 4);
            dub_rd.integ_q8 = 0;
            dub_rd.n = 0;
            dub_rd.starts++;
        } else {
            if (fill > DUB_UA_TARGET)                   /* (a stale backlog: keep the newest) */
                dub_ua_r = w - DUB_UA_TARGET;
            dub_rd.g = dub_rd.g < 64 ? 0 : dub_rd.g - (dub_rd.g >> 6);   /* fade out the held frame */
            *left = (dub_rd.last_l * dub_rd.g) >> 15;
            *right = (dub_rd.last_r * dub_rd.g) >> 15;
            dub_tel.usb_in_live = 0;
            return;
        }
    }
    if (fill < 2u) {                                    /* underrun: hold, fade, start again */
        dub_rd.go = 0;
        dub_rd.underruns++;
        dub_rd.g = dub_rd.g < 64 ? 0 : dub_rd.g - (dub_rd.g >> 6);
        *left = (dub_rd.last_l * dub_rd.g) >> 15;
        *right = (dub_rd.last_r * dub_rd.g) >> 15;
        dub_tel.usb_in_live = 0;
        return;
    }
    {
        uint32_t a = dub_ua_ring[r & (DUB_UA_N - 1u)], b = dub_ua_ring[(r + 1u) & (DUB_UA_N - 1u)];
        int32_t f = (int32_t)(dub_rd.frac >> 1);        /* Q15 */
        l = dub_ua_l(a) + (((dub_ua_l(b) - dub_ua_l(a)) * f) >> 15);
        rr = dub_ua_r16(a) + (((dub_ua_r16(b) - dub_ua_r16(a)) * f) >> 15);
    }
    dub_rd.frac += dub_rd.step;
    adv = dub_rd.frac >> 16;
    dub_rd.frac &= 0xFFFFu;
    RING_PUBLISH();                                     /* the slots read before the index frees them */
    dub_ua_r = r + adv;
    if ((++dub_rd.n & (DUB_UA_CTL - 1u)) == 0u)
        dub_ua_control(fill);
    dub_rd.g += (gt - dub_rd.g) >> 7;                   /* ~3 ms: no zipper on the fader, a fade on start */
    dub_rd.last_l = l;
    dub_rd.last_r = rr;
    *left = (l * dub_rd.g) >> 15;
    *right = (rr * dub_rd.g) >> 15;
    dub_tel.usb_in_live = 1;
}

#endif /* DUB_UAC_OUT */
