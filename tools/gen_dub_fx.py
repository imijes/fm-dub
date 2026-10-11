#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The dub preamp's 2:1 filter (firmware/src/dub_preamp/dub_effects.c, DUB_FX_FIR): prints the C table to paste
there. Not run by the build: the firmware holds the numbers, this shows where they come from.

The echo and the spring run at 22.05 kHz. Going down, this low-pass (20 taps, Kaiser beta 6, cut 7.5 kHz) is
evaluated once per two input samples (the decimator: nothing above 11.025 kHz folds back; -62 dB above 12 kHz);
coming back up, its two polyphase halves (even taps, odd taps) make the 44.1 kHz samples (the interpolator). It
is dark on purpose: -0.7 dB at 5 kHz, -8 dB at 8 kHz, as a tape echo.

Q15, symmetric (the decimator adds mirror pairs: 10 multiplies). The taps sum to 32768 exactly (DC gain 1 down);
a tap and its mirror lie in different polyphase halves, so each half sums to 16384 (x 2 up: DC gain 1).

  python3 tools/gen_dub_fx.py
"""
import math

FS, TAPS, FC, BETA = 44100, 20, 7500, 6.0


def i0(x):
    s = t = 1.0
    for k in range(1, 40):
        t *= (x / 2 / k) ** 2
        s += t
    return s


def design():
    h = []
    for n in range(TAPS):
        m = n - (TAPS - 1) / 2
        w = 2 * math.pi * FC / FS
        s = (w / math.pi) * (math.sin(w * m) / (w * m) if m else 1.0)
        h.append(s * i0(BETA * math.sqrt(1 - (2 * n / (TAPS - 1) - 1) ** 2)) / i0(BETA))
    tot = sum(h)
    half = [round(h[n] / tot * 32768) for n in range(TAPS // 2)]
    half[-1] += (16384 - sum(half))          # the centre pair takes the rounding: the sum is 32768 exactly
    q = half + half[::-1]                    # symmetric
    return q


def main():
    q = design()
    print(f"/* tools/gen_dub_fx.py: {TAPS} taps, {FC} Hz, Kaiser {BETA}, Q15 (sum {sum(q)}; halves "
          f"{sum(q[0::2])} / {sum(q[1::2])}) */")
    print(f"static const int16_t DUB_FX_FIR[DUB_FX_TAPS] = {{{', '.join(str(v) for v in q)}}};")


if __name__ == "__main__":
    main()
