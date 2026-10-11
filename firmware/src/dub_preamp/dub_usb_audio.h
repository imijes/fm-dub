/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB PREAMP, Phase 2.1: the USB audio OUTPUT of the computer (a UAC1 playback stream) into the FM-1.
 * Constants and descriptor bytes, included by usb.c (top); the code is dub_usb_audio.c (included by usb.c too).
 *
 * The device gains, next to SLOOP's recording stream (IF 2, EP4 IN, the master out):
 *   AC: input terminal 3 (USB streaming, 2 ch) -> output terminal 4 (speaker); the AC header lists IF 3 too
 *   IF 3 audio streaming: alt 0 no bandwidth, alt 1 = 16-bit stereo PCM, 44.1 kHz, EP4 OUT isochronous
 *        adaptive, 1 ms (the computer's clock; the FM-1 follows it by resampling, dub_usb_audio.c)
 * EP4 is used in both directions (IN 0x84 recording, OUT 0x04 playback), as the JieLi SDK pairs a UAC microphone
 * and speaker on one endpoint number (SDK MIC_ISO_EP_IN = SPK_ISO_EP_OUT = 3). CDC (USB SERIAL ON) moves to
 * IF 4-5. Needs FELUCCA_UAC (the audio control interface it shares). */
#pragma once

#if defined(ENABLE_DUB_PREAMP) && FELUCCA_UAC
#define DUB_UAC_OUT 1
#else
#define DUB_UAC_OUT 0
#endif

#if DUB_UAC_OUT
#define DUB_AS_IF 3u                  /* the playback streaming interface */
#define DUB_UA_RATE 44100u
#define DUB_UA_MAXF 48u               /* frames in the largest packet we accept: 44.1 sends 44 / 45 (margin) */
#define DUB_UA_MAXP (DUB_UA_MAXF * 4u)
#define DUB_CFG_EXTRA 74              /* bytes the descriptors gain: AC header +1, IT 3 + OT 4 21, IF 3 52 */
#define DUB_AC_TOTAL 53               /* AC class-specific total: header 11, IT1 12, OT2 9, IT3 12, OT4 9 */

/* the AC header (replaces SLOOP's 10-byte one: one more streaming interface) */
#define DUB_DESC_AC_HEADER \
    11, 0x24, 1, 0x00, 0x01, DUB_AC_TOTAL, 0, 3, 1, UAC_AS_IF, DUB_AS_IF,
/* after SLOOP's IT 1 / OT 2: the playback path */
#define DUB_DESC_AC_PLAY \
    12, 0x24, 2, 3, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 3: USB streaming, 2 ch (L R) */ \
    9, 0x24, 3, 4, 0x01, 0x03, 0, 3, 0,                 /* output terminal 4: speaker, from 3 */
/* IF 3 */
#define DUB_DESC_AS_PLAY \
    9, 4, DUB_AS_IF, 0, 0, 1, 2, 0, 0,                  /* IF3 audio streaming, alt 0: no bandwidth */ \
    9, 4, DUB_AS_IF, 1, 1, 1, 2, 0, 0,                  /* alt 1: the computer plays */ \
    7, 0x24, 1, 3, 1, 0x01, 0x00,                       /* AS general: terminal 3, delay 1, PCM */ \
    11, 0x24, 2, 1, 2, 2, 16, 1, DUB_UA_RATE & 0xFF, (DUB_UA_RATE >> 8) & 0xFF, DUB_UA_RATE >> 16, \
                                                        /* type I: 2 ch, 16 bit, 44100 */ \
    9, 5, 0x04, 0x09, DUB_UA_MAXP & 0xFF, DUB_UA_MAXP >> 8, 1, 0, 0,   /* EP4 OUT isochronous adaptive, 1 ms */ \
    7, 0x25, 1, 0x00, 0, 0, 0,                          /* CS endpoint: no controls */
#else
#define DUB_CFG_EXTRA 0
#endif
