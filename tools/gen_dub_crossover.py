#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The dub preamp crossover's coefficients (firmware/src/dub_preamp/dub_crossover.c, DUB_XO_COEF): prints the
C table to paste there. Not run by the build: the firmware holds the numbers, this shows where they come from.

Each split frequency fc gets two biquads (RBJ cookbook, bilinear, Q = 1/sqrt(2): Butterworth):
  LP  the 2nd-order Butterworth low-pass; two in cascade are the LR4 low-pass
  AP  the 2nd-order allpass with the same poles: LR4 low + LR4 high = AP exactly, so the firmware takes the
      LR4 high-pass as AP - LP4 (identical to two Butterworth high-passes in cascade, one biquad fewer, and the
      bands sum to the allpass to the last bit) and uses AP to align the other branch's phase.
Q30 (1.0 = 2^30: a1 is near -2). The LP numerators are trimmed so b0 + b1 + b2 = 2^30 + a1 + a2 exactly
(DC gain 1 to the bit); the AP's are its denominator reversed (|H| = 1 to the bit).

  python3 tools/gen_dub_crossover.py [FS]      (FS: 44100, core.h)
"""
import math
import sys

FS = int(sys.argv[1]) if len(sys.argv) > 1 else 44100
SPLITS = (80, 300, 3500)
ONE = 1 << 30


def design(fc):
    w = 2 * math.pi * fc / FS
    cw, alpha = math.cos(w), math.sin(w) / (2 * (1 / math.sqrt(2)))
    a0 = 1 + alpha
    a1 = round(-2 * cw / a0 * ONE)
    a2 = round((1 - alpha) / a0 * ONE)
    # low-pass: b = (1 - cos) / 2 * (1, 2, 1) / a0, the sum tied to the denominator's (DC gain exactly 1)
    tot = ONE + a1 + a2
    b0 = tot // 4
    b1 = tot - 2 * b0
    lp = (b0, b1, b0, a1, a2)
    ap = (a2, a1, ONE, a1, a2)              # allpass: (1 - alpha, -2 cos, 1 + alpha) / a0
    return lp, ap


def main():
    print(f"/* tools/gen_dub_crossover.py {FS}: b0, b1, b2, a1, a2 (Q30) */")
    print("static const dub_bq_coef_t DUB_XO_COEF[DUB_XO_NCOEF] = {")
    for fc in SPLITS:
        lp, ap = design(fc)
        for name, c in (("LP", lp), ("AP", ap)):
            print(f"    {{{', '.join(str(v) for v in c)}}},   /* {name} {fc} Hz */")
    print("};")


if __name__ == "__main__":
    main()
