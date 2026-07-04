#!/usr/bin/env python3
"""Full PHI_WEAVE pipeline simulation: physics, tables, scan, all fixes.
SYNC NOTE: firmware now applies (searched via this sim, 2026-07-03):
stiffness floor 0.016, damping floor 0.006, bowBalance = 0.60*min(1,
sqrt(d/0.0255)) per node, linear scan. Apply these on top of build() when
reproducing firmware behavior.
Reproduce the mid-Silk hash, then bisect."""

import numpy as np
import math

FS = 44100.0
BUF = 128
NN = 32

PHI = {
    "kPhiN100": 0.6180340,
    "kPhiN050": 0.7861513,
    "kPhiN025": 0.8872984,
    "kPhi025": 1.1271566,
    "kPhi033": 1.1746627,
    "kPhi050": 1.2720196,
    "kPhi067": 1.3871872,
    "kPhi075": 1.4352958,
    "kPhi100": 1.6180340,
    "kPhi125": 1.8257419,
    "kPhi150": 2.0581710,
    "kPhi175": 2.3197171,
    "kPhi200": 2.6180340,
    "kPhi225": 2.9603110,
    "kPhi250": 3.3302077,
    "kPhi275": 3.7515562,
    "kPhi300": 4.2360680,
    "kPhi325": 4.7742114,
    "kPhi350": 5.3884156,
    "kPhi375": 6.0409418,
}

CFG = {  # name: (phiFreq, duty, offset, bipolar)
    "StiffBase": ("kPhi125", 0.6, 0.000, False),
    "CoupleBase": ("kPhiN050", 0.7, 0.210, False),
    "DampBase": ("kPhi250", 0.5, 0.420, False),
    "StiffLand1": ("kPhi075", 0.8, 0.060, True),
    "StiffLand2": ("kPhi300", 0.5, 0.530, True),
    "DampLand1": ("kPhi175", 0.7, 0.140, True),
    "DampLand2": ("kPhiN025", 0.6, 0.660, True),
    "CoupleLand": ("kPhi225", 0.6, 0.310, True),
    "HomeP2": ("kPhi150", 0.6, 0.480, True),
    "HomeP3": ("kPhi325", 0.5, 0.760, True),
    "HomeP5": ("kPhi050", 0.35, 0.900, True),
    "HomePh2": ("kPhi025", 1.0, 0.320, False),
    "HomePh3": ("kPhi275", 1.0, 0.610, False),
    "BowDepth": ("kPhi350", 0.55, 0.190, False),
    "BowRate": ("kPhi100", 0.8, 0.370, False),
    "BowPos": ("kPhi200", 1.0, 0.740, False),
    "BowSpread": ("kPhiN100", 0.7, 0.880, False),
    "Travel": ("kPhi067", 0.5, 0.250, True),
    "PluckPos": ("kPhi375", 1.0, 0.090, False),
    "PluckWidth": ("kPhi033", 0.7, 0.440, False),
    "PluckAmp": ("kPhi250", 0.6, 0.820, False),
    "Shimmer": ("kPhi067", 0.6, 0.240, False),
    "MorphBow": ("kPhi175", 0.7, 0.150, False),
    "OutGain": ("kPhiN025", 0.9, 0.560, False),
    "StiffCycles": ("kPhi050", 1.0, 0.470, False),
    "DampCycles": ("kPhi325", 1.0, 0.720, False),
}


def tri_uni(ph, duty):
    ph = ph - math.floor(ph)
    hd = duty * 0.5
    if ph < hd:
        return ph * (2.0 / duty)
    if ph < duty:
        return (duty - ph) * (2.0 / duty)
    return 0.0


def tri_bi(ph, duty):
    ph = ph - math.floor(ph)
    qd, hd = duty * 0.25, duty * 0.5
    if ph < qd:
        return ph / qd
    if ph < hd:
        return (hd - ph) / qd
    if ph < hd + qd:
        return -(ph - hd) / qd
    if ph < duty:
        return -(duty - ph) / qd
    return 0.0


def ev(phase, name, fm=1.0):
    f, d, o, bi = CFG[name]
    f = PHI[f]
    w = ((phase + o) * f * fm) % 1.0
    return tri_bi(w, d) if bi else tri_uni(w, d)


def ev_spatial(phase, nf, cyc, name):
    f, d, o, bi = CFG[name]
    f = PHI[f]
    w = (phase * f + nf * cyc + o) % 1.0
    return tri_bi(w, d) if bi else tri_uni(w, d)


def build(zone):
    ph = zone / 1023.0
    p = {}
    stiffT = ev(ph, "StiffBase")
    stiffBase = 0.0008 * 75.0**stiffT
    coupleT = ev(ph, "CoupleBase")
    coupleBase = 0.010 + coupleT * coupleT * 0.28
    dampT = ev(ph, "DampBase")
    dampBase = 0.0015 + dampT * dampT * 0.0435
    p2 = ev(ph, "HomeP2") * 0.7
    p3 = ev(ph, "HomeP3") * 0.55
    p5 = ev(ph, "HomeP5") * 0.4
    ph2 = ev(ph, "HomePh2") * 2 * math.pi
    ph3 = ev(ph, "HomePh3") * 2 * math.pi
    sc = 1.0 + ev(ph, "StiffCycles") * 3.0
    dc = 1.0 + ev(ph, "DampCycles") * 3.0
    p["k"] = np.zeros(NN)
    p["c"] = np.zeros(NN)
    p["d"] = np.zeros(NN)
    p["home"] = np.zeros(NN)
    hp = 0.0
    for i in range(NN):
        nf = i / NN
        sl = (
            1.0
            + 0.8 * ev_spatial(ph, nf, sc, "StiffLand1")
            + 0.5 * ev_spatial(ph, nf, sc * 2, "StiffLand2")
        )
        p["k"][i] = np.clip(stiffBase * sl, 0.0002, 0.12)
        dl = (
            1.0
            + 0.85 * ev_spatial(ph, nf, dc, "DampLand1")
            + 0.5 * ev_spatial(ph, nf, dc * 3, "DampLand2")
        )
        p["d"][i] = np.clip(dampBase * dl, 0.0005, 0.09)
        cl = 1.0 + 0.6 * ev_spatial(ph, nf, sc, "CoupleLand")
        p["c"][i] = np.clip(coupleBase * cl, 0.004, 0.45)
        h = (
            math.sin(2 * math.pi * nf)
            + p2 * math.sin(4 * math.pi * nf + ph2)
            + p3 * math.sin(6 * math.pi * nf + ph3)
            + p5 * math.sin(10 * math.pi * nf)
        )
        p["home"][i] = h
        hp = max(hp, abs(h))
    if hp > 0.01:
        p["home"] /= hp
    bdT = ev(ph, "BowDepth")
    p["bowDepth"] = 0.0015 + bdT * bdT * 0.030
    p["bowRate"] = 0.002 + ev(ph, "BowRate") * 0.045
    p["bowPos"] = ev(ph, "BowPos")
    p["bowSpread"] = 0.06 + ev(ph, "BowSpread") * 0.20
    tT = ev(ph, "Travel")
    p["travel"] = tT**3 * 0.004
    p["pluckPos"] = ev(ph, "PluckPos")
    p["pluckWidth"] = 0.03 + ev(ph, "PluckWidth") * 0.20
    p["pluckAmp"] = 0.4 + ev(ph, "PluckAmp") * 0.6
    p["outGain"] = 0.70 + ev(ph, "OutGain") * 0.55
    p["shimmer"] = 0.07 * 4.3 ** ev(ph, "Shimmer")
    return p


class Sim:
    def __init__(s, zone, seed=42):
        s.p = build(zone)
        s.x = np.zeros(NN)
        s.v = np.zeros(NN)
        s.xs1 = np.zeros(NN)
        s.xs = np.zeros(NN)
        s.bowPhase = 0.0
        s.travelPhase = 0.0
        s.agcPeak = 0.0
        s.agcScale = 0.0
        s.rng = np.random.default_rng(seed)
        s.pluck()

    def pluck(s):
        p = s.p
        for i in range(NN):
            nf = i / NN
            pd = nf - p["pluckPos"]
            pd -= math.floor(pd + 0.5)
            pw = abs(pd) / p["pluckWidth"]
            if pw < 1.0:
                s.x[i] += p["pluckAmp"] * (0.5 + 0.5 * math.cos(math.pi * pw))

    def step(s):
        p = s.p
        s.bowPhase = (s.bowPhase + p["bowRate"] * 0.5) % 1.0
        bowVal = (tri_uni(s.bowPhase, 1.0) * 2 - 1) * p["bowDepth"]
        nf = np.arange(NN) / NN
        bd = nf - p["bowPos"]
        bd -= np.floor(bd + 0.5)
        bw = np.abs(bd) / p["bowSpread"]
        bowF = np.where(
            bw < 1.0, bowVal * (0.5 + 0.5 * np.cos(np.pi * np.clip(bw, 0, 1))), 0.0
        )
        xm1 = np.roll(s.x, 1)
        xp1 = np.roll(s.x, -1)
        accel = (
            0.25 * (p["c"] * (xm1 + xp1 - 2 * s.x) - p["k"] * (s.x - p["home"]) + bowF)
            - 0.5 * p["d"] * s.v
        )
        s.v = np.clip(s.v + accel, -0.25, 0.25)
        s.x = np.clip(s.x + s.v, -2.0, 2.0)
        s.xs1 += p["shimmer"] * (s.x - s.xs1)
        s.xs += p["shimmer"] * (s.xs1 - s.xs)
        peak = np.abs(s.xs).max()
        s.agcPeak = max(peak, s.agcPeak * 0.999)
        target = p["outGain"] * 1.0 / max(s.agcPeak, 0.35)  # ref amp normalized to 1.0
        if s.agcScale == 0.0:
            s.agcScale = target
        s.agcScale += 0.10 * (target - s.agcScale)
        s.travelPhase = (s.travelPhase + p["travel"] * 0.5) % 1.0
        return s.xs * s.agcScale


def cr_scan(tab, phase01):
    # tab: 32 values; catmull-rom, circular
    fpos = phase01 * NN
    i = int(fpos) % NN
    t = fpos - int(fpos)
    p0, p1, p2, p3 = tab[(i - 1) % NN], tab[i], tab[(i + 1) % NN], tab[(i + 2) % NN]
    return p1 + 0.5 * t * (
        (p2 - p0)
        + t * ((2 * p0 - 5 * p1 + 4 * p2 - p3) + t * (3 * (p1 - p2) + p3 - p0))
    )


def smoothstep(t):
    return t * t * (3 - 2 * t)


def render(zone, f0, seconds, disable=()):
    s = Sim(zone)
    if "bow" in disable:
        s.p["bowDepth"] = 0.0
    if "travel" in disable:
        s.p["travel"] = 0.0
    if "agc" in disable:
        pass
    nbuf = int(seconds * FS / BUF)
    out = np.zeros(nbuf * BUF)
    prev = s.step().copy()
    mid = s.step().copy()
    cur = s.step().copy()
    phase = 0.0
    travelAcc = 0.0
    tprev = 0.0
    for b in range(nbuf):
        prev = cur.copy()
        mid = s.step().copy()
        cur = s.step().copy()
        if "freeze" in disable and b > 20:
            mid = prev.copy()
            cur = prev.copy()
        tnow = s.travelPhase
        tdelta = tnow - tprev
        tdelta -= round(tdelta)  # signed wrap
        if "agc" in disable:
            sc = 1.0
        for n in range(BUF):
            half = BUF // 2
            if n < half:
                a, bb, tf = prev, mid, smoothstep((n + 1) / half)
            else:
                a, bb, tf = mid, cur, smoothstep((n - half + 1) / half)
            travelAcc += tdelta / BUF
            phase += f0 / FS
            sp = (phase + tprev + travelAcc) % 1.0
            wa = cr_scan(a, sp)
            wb = cr_scan(bb, sp)
            out[b * BUF + n] = wa + (wb - wa) * tf
        tprev = tnow
    return out


def inharmonicity(sig, f0):
    n = len(sig)
    w = np.hanning(n)
    sp = np.abs(np.fft.rfft(sig * w))
    freqs = np.fft.rfftfreq(n, 1 / FS)
    total = (sp**2).sum()
    harm = 0.0
    for h in range(1, int(20000 / f0)):
        fh = h * f0
        idx = np.argmin(np.abs(freqs - fh))
        lo, hi = max(0, idx - 6), idx + 7
        harm += (sp[lo:hi] ** 2).sum()
    return 10 * math.log10(max(total - harm, 1e-12) / total)


f0 = 82.4
for zone in (64, 40, 90, 576, 700):
    sig = render(zone, f0, 2.0)[FS.__int__() // 2 :]
    print(f"zone {zone:4d}: inharmonic/total = {inharmonicity(sig, f0):6.1f} dB")
