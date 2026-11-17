#!/usr/bin/env python3
"""Fits a parametric-EQ model (highpass, low shelf, bells, lowpass) to a
response curve traced from a Pro-Q 3 screenshot, at fs = 48 kHz (the chain's
internal rate), using the same RBJ biquads as native/src/biquad.cpp.

usage: fit_proq_curve.py curve.json   (list of [freq_hz, gain_db])
"""
import cmath, json, math, random, sys

FS = 48000.0

def biquad_response(b, a, f):
    z1 = cmath.exp(-1j * 2 * math.pi * f / FS)
    z2 = z1 * z1
    return (b[0] + b[1] * z1 + b[2] * z2) / (1 + a[0] * z1 + a[1] * z2)

def norm(b0, b1, b2, a0, a1, a2):
    return (b0 / a0, b1 / a0, b2 / a0), (a1 / a0, a2 / a0)

def lowpass(f0, q):
    w = 2 * math.pi * f0 / FS; c = math.cos(w); al = math.sin(w) / (2 * q)
    return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al)

def highpass(f0, q):
    w = 2 * math.pi * f0 / FS; c = math.cos(w); al = math.sin(w) / (2 * q)
    return norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al)

def peaking(f0, q, g):
    A = 10 ** (g / 40); w = 2 * math.pi * f0 / FS; c = math.cos(w); al = math.sin(w) / (2 * q)
    return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A)

def lowshelf(f0, g):
    A = 10 ** (g / 40); w = 2 * math.pi * f0 / FS; c = math.cos(w)
    al = math.sin(w) / 2 * math.sqrt(2); t = 2 * math.sqrt(A) * al
    return norm(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - t),
                (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t)

def butter_sections(order, f0, hp):
    out = []
    for k in range(1, order // 2 + 1):
        q = 1 / (2 * math.cos((2 * k - 1) * math.pi / (2 * order)))
        out.append(highpass(f0, q) if hp else lowpass(f0, q))
    return out

def model_db(p, f):
    secs = butter_sections(p["hp_order"], p["hp_f"], True) + butter_sections(p["lp_order"], p["lp_f"], False)
    secs.append(lowshelf(p["ls_f"], p["ls_g"]))
    for (bf, bg, bq) in p["bells"]:
        secs.append(peaking(bf, bq, bg))
    h = 1
    for b, a in secs:
        h *= biquad_response(b, a, f)
    return 20 * math.log10(abs(h) + 1e-12) + p["offset"]

def error(p, pts):
    return sum((model_db(p, f) - d) ** 2 for f, d in pts) / len(pts)

def clone(p):
    q = dict(p); q["bells"] = [list(b) for b in p["bells"]]; return q

def fit(pts, init, iters=60):
    p = clone(init); best = error(p, pts)
    for it in range(iters):
        scale = 0.5 ** (it / 12)
        improved = False
        def try_set(getter, setter, delta, lo=None, hi=None):
            nonlocal best, improved
            for s in (+1, -1):
                v = getter() + s * delta
                if lo is not None and v < lo or hi is not None and v > hi: continue
                old = getter(); setter(v)
                e = error(p, pts)
                if e < best - 1e-9: best = e; improved = True; return
                setter(old)
        try_set(lambda: p["hp_f"], lambda v: p.__setitem__("hp_f", v), 4 * scale, 40, 200)
        try_set(lambda: p["lp_f"], lambda v: p.__setitem__("lp_f", v), 300 * scale, 9000, 21000)
        try_set(lambda: p["ls_f"], lambda v: p.__setitem__("ls_f", v), 10 * scale, 40, 400)
        try_set(lambda: p["ls_g"], lambda v: p.__setitem__("ls_g", v), 0.3 * scale, -9, 9)
        try_set(lambda: p["offset"], lambda v: p.__setitem__("offset", v), 0.3 * scale, -12, 6)
        for b in p["bells"]:
            try_set(lambda b=b: b[0], lambda v, b=b: b.__setitem__(0, v), b[0] * 0.04 * scale, 80, 15000)
            try_set(lambda b=b: b[1], lambda v, b=b: b.__setitem__(1, v), 0.4 * scale, -18, 6)
            try_set(lambda b=b: b[2], lambda v, b=b: b.__setitem__(2, v), b[2] * 0.12 * scale, 0.3, 12)
        if not improved and scale < 0.05: break
    return p, best

if __name__ == "__main__":
    curve = json.load(open(sys.argv[1]))
    pts = [(f, d) for f, d in curve if f >= 55 and f <= 16500 and d > -23][::2]
    bells0 = [[200, -5.6, 1.0], [600, -2.7, 1.0], [1085, -6.2, 1.5], [1770, 0.0, 1.0],
              [3300, -3.8, 1.5], [4000, -6.5, 5.0], [6900, -7.8, 0.8]]
    best = None
    for hp_order in (2, 4, 6, 8):
        for lp_order in (4, 8):
            init = {"hp_order": hp_order, "hp_f": 97, "lp_order": lp_order, "lp_f": 15500, "ls_f": 130, "ls_g": -1.5,
                    "bells": bells0, "offset": 0.0}
            p, e = fit(pts, init)
            print(f"hp order {hp_order} lp order {lp_order}: rms error {math.sqrt(e):.2f} dB")
            if best is None or e < best[1]: best = (p, e)
    p, e = best
    print("\nBEST rms error %.2f dB" % math.sqrt(e))
    print(json.dumps(p, indent=1))
    print("\n freq    target   model")
    for f, d in pts[::25]:
        print(f"{f:7.0f} {d:7.1f} {model_db(p, f):7.1f}")
