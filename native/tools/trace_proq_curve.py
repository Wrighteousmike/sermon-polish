#!/usr/bin/env python3
"""Traces the yellow EQ response curve out of a Pro-Q 3 screenshot and writes
[[freq_hz, gain_db], ...] JSON for fit_proq_curve.py.

Axis calibration is specific to the screenshot in docs/protools-reference-chain.md
(1253x736): x = 783 at 1 kHz with 282.5 px per decade, y = 343 at 0 dB with
15.33 px per dB, both read off the plugin's own axis labels.

usage: trace_proq_curve.py screenshot.png out.json
"""
import json, sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
pts = []
for x in range(215, 1145):
    ys = [y for y in range(360, 700) if (lambda r, g, b: r > 225 and 160 < g < 225 and b < 90)(*px[x, y])]
    if ys:
        pts.append((1000 * 10 ** ((x - 783) / 282.5), -(sum(ys) / len(ys) - 343) / 15.33))
json.dump(pts, open(sys.argv[2], "w"))
print(len(pts), "points")
