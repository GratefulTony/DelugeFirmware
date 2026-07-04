#!/usr/bin/env python3
"""Generate figures for the PHI_STAIR documentation (phi-stair-steps.md).

Ports the firmware's builder and table generation: pattern families, the
slope dimension, and the continuous fractional step count.
"""

import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

OUTPUT_DIR = Path(__file__).parent / "images"
OUTPUT_DIR.mkdir(exist_ok=True)

PHI = {
    "kPhiN050": 0.7861513,
    "kPhi050": 1.2720196,
    "kPhi125": 1.8257419,
    "kPhi175": 2.3197171,
    "kPhi200": 2.6180340,
    "kPhi250": 3.3302077,
    "kPhi275": 3.7515562,
    "kPhi300": 4.2360680,
    "kPhi325": 4.7742114,
}
CFG = {
    "Count": ("kPhi125", 0.7, 0.000, False),
    "Slope": ("kPhi250", 0.6, 0.140, False),
    "HeightLand": ("kPhi275", 0.6, 0.290, True),
    "Tilt": ("kPhiN050", 0.7, 0.410, True),
    "Asym": ("kPhi175", 0.6, 0.550, False),
    "AsymLand": ("kPhi325", 0.5, 0.660, True),
    "LandCycles": ("kPhi050", 1.0, 0.780, False),
}


def tri_u(ph, d):
    ph -= math.floor(ph)
    hd = d * 0.5
    if ph < hd:
        return ph * (2 / d)
    if ph < d:
        return (d - ph) * (2 / d)
    return 0.0


def tri_b(ph, d):
    ph -= math.floor(ph)
    qd, hd = d * 0.25, d * 0.5
    if ph < qd:
        return ph / qd
    if ph < hd:
        return (hd - ph) / qd
    if ph < hd + qd:
        return -(ph - hd) / qd
    if ph < d:
        return -(d - ph) / qd
    return 0.0


def ev(phase, name):
    f, d, o, b = CFG[name]
    f = PHI[f]
    w = ((phase + o) * f) % 1.0
    return tri_b(w, d) if b else tri_u(w, d)


def spatial(phase, t, cycles, freq, duty, off, bipolar):
    w = (phase * PHI[freq] + t * cycles + off) % 1.0
    return tri_b(w, duty) if bipolar else tri_u(w, duty)


def pattern(family, t, phase):
    if family == 0:
        return -0.8 if (int(t * 16) & 1) else 0.8
    if family == 1:
        return t * 2 - 1
    if family == 2:
        return t * 1.6 - 0.8
    if family == 3:
        return (
            (t / 0.3) if t < 0.3 else (1.0 if t < 0.7 else 1 - (t - 0.7) / 0.3)
        ) * 1.6 - 0.8
    if family == 4:
        return 0.9 if abs(t - 0.5) < 0.2 else -0.45
    if family == 5:
        return round(spatial(phase, t, 7.0, "kPhi300", 0.9, 0.230, True) * 3) / 3
    if family == 6:
        return spatial(phase, t, 3.0, "kPhi200", 0.5, 0.510, True)
    return (0.7 if (int(t * 16) & 2) else -0.7) + t * 0.5 - 0.25


def build(zone, count_override=None, slope_override=None):
    phase = zone / 1023.0
    family = min(7, zone >> 7)
    count = count_override if count_override else 2 + ev(phase, "Count") * 14
    slope = ev(phase, "Slope") ** 2 * 0.9
    if family == 2:
        slope = 0.3 + slope * 0.6
    if slope_override is not None:
        slope = slope_override
    tilt = ev(phase, "Tilt") * 0.6
    asym = ev(phase, "Asym")
    cycles = 1 + ev(phase, "LandCycles") * 3
    h, w = [], []
    peak = 1e-4
    for i in range(16):
        t = (i + 0.5) / count
        hv = (
            pattern(family, min(t, 1.0), phase)
            + 0.35
            * spatial(phase, t, cycles, *CFG["HeightLand"][:1], *CFG["HeightLand"][1:])
            + tilt * (t * 2 - 1)
        )
        h.append(hv)
        peak = max(peak, abs(hv))
        wl = 1 + asym * 0.85 * spatial(
            phase, t, cycles * 2, *CFG["AsymLand"][:1], *CFG["AsymLand"][1:]
        )
        w.append(max(wl, 0.1) * max(0.0, min(count - i, 1.0)))
    h = [x / peak for x in h]
    return h, w, slope


def table(h, w, slope, slots=512):
    wsum = sum(w)
    wn = [x / wsum for x in w]
    last = max(i for i in range(16) if w[i] > 0)
    out = np.zeros(slots)
    seg, seg_start, seg_w = 0, 0.0, wn[0]
    for j in range(slots):
        u = j / slots
        while u >= seg_start + seg_w and seg < 15:
            seg_start += seg_w
            seg += 1
            seg_w = wn[seg]
        hp = h[last] if seg == 0 else h[seg - 1]
        val = h[seg]
        if seg_w > 0 and slope > 0.001:
            frac = (u - seg_start) / seg_w
            if frac < slope:
                val = hp + (h[seg] - hp) * (frac / slope)
        out[j] = val
    out -= out.mean()
    rms = np.sqrt((out**2).mean())
    return out * min(0.55 / max(rms, 0.05), 0.95 / max(np.abs(out).max(), 1e-4))


NAMES = ["Brick", "Terrace", "Ramp", "Mesa", "Pylon", "Glyph", "Shard", "Teeth"]


def fig1():
    fig, axes = plt.subplots(2, 4, figsize=(10, 4.2), sharey=True)
    for f in range(8):
        ax = axes[f // 4][f % 4]
        zone = f * 128 + 64
        ax.plot(table(*build(zone)), linewidth=0.9, color="#36c")
        ax.set_title(f"{NAMES[f]} (z{zone})", fontsize=9)
        ax.set_xticks([])
    fig.suptitle(
        "PHI_STAIR pattern families, one cycle each (mid-zone positions)", fontsize=10
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"stair_fig1_families.{ext}", dpi=110)
    plt.close(fig)


def fig2():
    fig, axes = plt.subplots(1, 3, figsize=(9.5, 3.0), sharey=True)
    for ax, sl in zip(axes, (0.0, 0.35, 0.85)):
        ax.plot(
            table(*build(320, slope_override=sl)[:2], sl), linewidth=0.9, color="#63a"
        )
        ax.set_title(f"slope {sl:.2f}", fontsize=9)
        ax.set_xticks([])
    fig.suptitle(
        "The slope dimension: hard edges (pulse-class bright) to ramped risers (mellow)",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"stair_fig2_slope.{ext}", dpi=110)
    plt.close(fig)


def fig3():
    fig, axes = plt.subplots(1, 3, figsize=(9.5, 3.0), sharey=True)
    for ax, c in zip(axes, (4.0, 4.5, 5.0)):
        ax.plot(table(*build(192, count_override=c)), linewidth=0.9, color="#a53")
        ax.set_title(f"count {c:.1f}", fontsize=9)
        ax.set_xticks([])
    fig.suptitle(
        "Continuous step count: the fractional last step fades in - sweeps never pop",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"stair_fig3_count.{ext}", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    print(f"Figures written to {OUTPUT_DIR}")
