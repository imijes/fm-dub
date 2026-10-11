#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The dub preamp's stepped high-pass (firmware/src/dub_preamp/dub_tubby_filter.c, DUB_TUBBY_COEF): prints the
C table to paste there. Not run by the build: the firmware holds the numbers, this shows where they come from.

After the Altec 9069B King Tubby used as his "big knob": a high-pass in steps, 18 dB / octave. Each step is a
resonant 2-pole high-pass (RBJ cookbook, bilinear; Q above Butterworth, chosen so the whole step, the 1-pole
included, peaks +4 dB at 70 Hz rising to +6 dB at 7.5 kHz: the ringing at the cutoff) followed by a 1-pole high-pass at the same cutoff (bilinear). Step 0 is FLAT: the firmware does
not run the filter at all (its row is the identity, kept so the table is indexed by the step).

Q30 (1.0 = 2^30). Columns: b0, b1, b2, a1, a2 (the biquad, a0 = 1), g, p (the 1-pole: y = g (x - x1) + p y1).

  python3 tools/gen_dub_tubby.py [FS]      (FS: 44100, core.h)
"""
import math
import sys

FS = int(sys.argv[1]) if len(sys.argv) > 1 else 44100
ONE = 1 << 30
#             FLAT   1     2     3     4     5      6      7      8      9      10
STEPS_HZ = (None, 70, 100, 150, 250, 500, 1000, 2000, 3000, 5000, 7500)
STEPS_Q = (None, 2.1, 2.16, 2.22, 2.29, 2.36, 2.43, 2.49, 2.57, 2.64, 2.71)   # peaks +4 dB (70 Hz) .. +6 dB (7.5 kHz)


def step(fc, q):
    w = 2 * math.pi * fc / FS
    cw, alpha = math.cos(w), math.sin(w) / (2 * q)
    a0 = 1 + alpha
    b0 = (1 + cw) / 2 / a0
    bq = (round(b0 * ONE), round(-2 * b0 * ONE), round(b0 * ONE), round(-2 * cw / a0 * ONE),
          round((1 - alpha) / a0 * ONE))
    k = math.tan(w / 2)
    g, p = 1 / (1 + k), (1 - k) / (1 + k)
    return bq + (round(g * ONE), round(p * ONE))


def main():
    print(f"/* tools/gen_dub_tubby.py {FS}: b0, b1, b2, a1, a2, g, p (Q30) */")
    print("static const dub_tubby_coef_t DUB_TUBBY_COEF[DUB_TUBBY_NSTEP] = {")
    print(f"    {{{{{ONE}, 0, 0, 0, 0}}, {ONE}, 0}},   /* 0 FLAT: never run */")
    for i in range(1, len(STEPS_HZ)):
        c = step(STEPS_HZ[i], STEPS_Q[i])
        print(f"    {{{{{', '.join(str(v) for v in c[:5])}}}, {c[5]}, {c[6]}}},   /* {i}: {STEPS_HZ[i]} Hz, Q {STEPS_Q[i]} */")
    print("};")


if __name__ == "__main__":
    main()
