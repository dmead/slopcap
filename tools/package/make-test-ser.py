#!/usr/bin/env python3
"""A small recording to try a package with: a drifting disc, 8-bit mono SER, 120 frames.
   make-test-ser.py OUT.ser   (then: slopcap --camera sim:OUT.ser)"""
import math
import struct
import sys

w, h, n = 160, 120, 120
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<14s7i40s40s40sqq", b"LUCAM-RECORDER", 0, 0, 0, w, h, 8, n, b"test", b"synthetic", b"", 0, 0))
    for k in range(n):
        cx, cy = 80 + 15 * math.sin(k / 15), 60
        f.write(bytes(220 if (x - cx) ** 2 + (y - cy) ** 2 < 400 else 12 for y in range(h) for x in range(w)))
