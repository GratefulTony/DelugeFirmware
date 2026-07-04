#!/usr/bin/env python3
"""Generate figures for the PHI_WEAVE physics documentation (phi-weave-physics.md).

Simulates the exact physics used by dsp/phi_weave.cpp (32-node mass-spring ring,
leapfrog integration, dt = 1 tick) and renders the figures referenced in the doc.
"""

import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent / "images"
OUTPUT_DIR.mkdir(exist_ok=True)

N = 32
PHI = 1.6180340


# ----------------------------------------------------------------------------
# Firmware-equivalent physics
# ----------------------------------------------------------------------------


def make_ring(k=0.01, c=0.08, d=0.008):
    """Uniform parameter profiles (arrays allow landscapes)."""
    return {
        "k": np.full(N, k),
        "c": np.full(N, c),
        "d": np.full(N, d),
        "home": np.sin(2 * np.pi * np.arange(N) / N),
    }


def tick(x, v, p, bow=0.0):
    """One leapfrog tick, exactly as tickPhiWeave() does it."""
    xm1 = np.roll(x, 1)
    xp1 = np.roll(x, -1)
    a = p["c"] * (xm1 + xp1 - 2 * x) - p["k"] * (x - p["home"]) - p["d"] * v + bow
    v = np.clip(v + a, -0.5, 0.5)
    x = np.clip(x + v, -2.0, 2.0)
    return x, v


def pluck(x, pos=0.3, width=0.12, amp=0.9):
    nf = np.arange(N) / N
    pd = nf - pos
    pd -= np.floor(pd + 0.5)  # wrap to [-0.5, 0.5)
    pw = np.abs(pd) / width
    bump = np.where(pw < 1.0, amp * (0.5 + 0.5 * np.cos(np.pi * pw)), 0.0)
    return x + bump


def triangle(phase, duty=1.0):
    phase = phase - np.floor(phase)
    half = duty * 0.5
    out = np.zeros_like(phase)
    rising = phase < half
    falling = (phase >= half) & (phase < duty)
    out[rising] = phase[rising] / half
    out[falling] = (duty - phase[falling]) / half
    return out


# ----------------------------------------------------------------------------
# Figure 1: The two clocks — slow physics scanned at audio rate
# ----------------------------------------------------------------------------


def fig1():
    p = make_ring()
    x = pluck(np.zeros(N), pos=0.55, width=0.10, amp=0.8)
    v = np.zeros(N)
    for _ in range(9):
        x, v = tick(x, v, p)

    fig = plt.figure(figsize=(10, 3.6))

    # Left: ring with node displacements (polar)
    ax1 = fig.add_subplot(1, 2, 1, projection="polar")
    theta = 2 * np.pi * np.arange(N + 1) / N
    r = 1.0 + 0.35 * np.append(x, x[0])
    ax1.plot(theta, r, "o-", markersize=3, linewidth=1.2, color="#2a6")
    ax1.plot(theta, np.ones(N + 1), ":", linewidth=0.8, color="#999")
    scan_angle = 2 * np.pi * 0.22
    ax1.annotate(
        "",
        xy=(scan_angle, 1.55),
        xytext=(scan_angle, 0.0),
        arrowprops=dict(arrowstyle="->", color="#c33", lw=1.5),
    )
    ax1.set_rticks([])
    ax1.set_xticks([])
    ax1.set_title(
        "32-node ring (control rate: ~344 ticks/s)\nred arrow = scan head", fontsize=9
    )

    # Right: the scanned output at audio rate (three scan cycles)
    ax2 = fig.add_subplot(1, 2, 2)
    scan_phase = np.linspace(0, 3, 3 * 256)
    idx = (scan_phase * N).astype(int) % N
    frac = (scan_phase * N) % 1.0
    wave = x[idx] * (1 - frac) + x[(idx + 1) % N] * frac
    ax2.plot(scan_phase, wave, linewidth=1.2, color="#36c")
    ax2.set_xlabel("scan cycles (audio rate: pitch = scan speed)")
    ax2.set_ylabel("output")
    ax2.set_title("Same ring, scanned as a wavetable", fontsize=9)
    ax2.grid(alpha=0.3)

    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"weave_fig1_two_clocks.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 2: Pluck relaxation — waveform snapshots over ticks
# ----------------------------------------------------------------------------


def fig2():
    p = make_ring(k=0.012, c=0.10, d=0.006)
    x = pluck(np.zeros(N), pos=0.3, width=0.12, amp=1.0)
    v = np.zeros(N)

    snap_at = [0, 3, 8, 16, 32, 64, 128, 300]
    snaps = {}
    t = 0
    snaps[0] = x.copy()
    for target in snap_at[1:]:
        while t < target:
            x, v = tick(x, v, p)
            t += 1
        snaps[target] = x.copy()

    fig, ax = plt.subplots(figsize=(8, 5))
    nodes = np.arange(N)
    for row, t in enumerate(snap_at):
        offset = -row * 1.6
        ax.plot(
            nodes,
            snaps[t] + offset,
            "-",
            linewidth=1.1,
            color=plt.cm.viridis(row / (len(snap_at) - 1)),
        )
        ax.axhline(offset, color="#ccc", linewidth=0.5, zorder=0)
        ax.text(N + 0.5, offset, f"tick {t}", fontsize=8, va="center")
    ax.plot(nodes, p["home"] + -(len(snap_at)) * 1.6, "--", color="#c33", linewidth=1.0)
    ax.text(
        N + 0.5,
        -(len(snap_at)) * 1.6,
        "home shape",
        fontsize=8,
        va="center",
        color="#c33",
    )
    ax.set_xlabel("node index (= position in the scanned cycle)")
    ax.set_yticks([])
    ax.set_xlim(0, N + 6)
    ax.set_title(
        "Pluck → traveling ripples → relaxation toward the home shape\n"
        "(one row per snapshot; ~344 ticks per second)",
        fontsize=9,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"weave_fig2_pluck_relaxation.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 3: Phi-painted physics landscapes (three zone positions)
# ----------------------------------------------------------------------------


def fig3():
    nf = np.arange(N) / N
    zones = [0.13, 0.55, 0.86]
    fig, axes = plt.subplots(2, 3, figsize=(10, 4.6), sharex=True)
    for col, z in enumerate(zones):
        stiff_cycles = 1.0 + triangle(np.array([z * PHI**0.5 + 0.47]))[0] * 3.0
        land = (
            1.0
            + 0.8 * (2 * triangle(z * PHI**0.75 + nf * stiff_cycles + 0.06, 0.8) - 1)
            + 0.5 * (2 * triangle(z * PHI**3.0 + nf * stiff_cycles * 2 + 0.53, 0.5) - 1)
        )
        k = np.clip(0.01 * land, 0.0002, 0.12)
        axes[0, col].fill_between(nf, 0, k, color="#36c", alpha=0.55)
        axes[0, col].set_title(f"zone position {z:.2f}", fontsize=9)
        axes[0, col].set_ylabel("stiffness" if col == 0 else "")

        damp_cycles = 1.0 + triangle(np.array([z * PHI**3.25 + 0.72]))[0] * 3.0
        dland = (
            1.0
            + 0.85 * (2 * triangle(z * PHI**1.75 + nf * damp_cycles + 0.14, 0.7) - 1)
            + 0.5
            * (2 * triangle(z * PHI**-0.25 + nf * damp_cycles * 3 + 0.66, 0.6) - 1)
        )
        d = np.clip(0.008 * dland, 0.0005, 0.09)
        axes[1, col].fill_between(nf, 0, d, color="#c63", alpha=0.55)
        axes[1, col].set_xlabel("position around ring")
        axes[1, col].set_ylabel("damping" if col == 0 else "")
    fig.suptitle(
        "Zone position paints per-node physics terrain (φ-triangle landscapes)",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"weave_fig3_landscapes.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 4: Leapfrog stability region and the firmware's parameter box
# ----------------------------------------------------------------------------


def fig4():
    fig, ax = plt.subplots(figsize=(6.4, 4.4))
    k = np.linspace(0, 1.2, 300)
    c_limit = (4.0 - k) / 4.0
    ax.fill_between(k, 0, c_limit, color="#9d9", alpha=0.5, label="stable: k + 4c < 4")
    ax.fill_between(k, c_limit, 1.2, color="#d99", alpha=0.5, label="unstable")
    # Firmware parameter box (clamped ranges from phi_weave.cpp)
    ax.add_patch(
        plt.Rectangle(
            (0.0002, 0.004),
            0.12 - 0.0002,
            0.45 - 0.004,
            fill=False,
            edgecolor="#136",
            linewidth=2,
        )
    )
    ax.annotate(
        "PHI_WEAVE\nparameter box\n(k ≤ 0.12, c ≤ 0.45\n→ ω² ≤ 1.92)",
        xy=(0.12, 0.45),
        xytext=(0.45, 0.6),
        fontsize=8,
        arrowprops=dict(arrowstyle="->", lw=1),
    )
    ax.set_xlabel("stiffness k")
    ax.set_ylabel("coupling c")
    ax.set_xlim(0, 1.2)
    ax.set_ylim(0, 1.1)
    ax.legend(fontsize=8, loc="upper right")
    ax.set_title("Leapfrog (dt = 1 tick) stability: ω²·dt² = (k + 4c) < 4", fontsize=9)
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"weave_fig4_stability.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 5: Morph-bow — crossfade motion injects energy
# ----------------------------------------------------------------------------


def fig5():
    rng = np.random.default_rng(7)
    ticks = 1000
    p_a = make_ring(k=0.010, c=0.08, d=0.010)
    p_b = make_ring(k=0.035, c=0.16, d=0.010)
    p_b["home"] = np.sin(2 * np.pi * np.arange(N) / N) + 0.5 * np.sin(
        6 * np.pi * np.arange(N) / N
    )

    def run(cf_curve, morph_bow_gain=4.5):
        x = np.zeros(N) + p_a["home"]
        v = np.zeros(N)
        # Settle to equilibrium first so only morph-bow energy is visible
        for _ in range(600):
            x, v = tick(x, v, p_a)
        energy = np.zeros(ticks)
        prev_cf = cf_curve[0]
        for t in range(ticks):
            cf = cf_curve[t]
            p = {
                key: (1 - cf) * p_a[key] + cf * p_b[key]
                for key in ("k", "c", "d", "home")
            }
            bow = rng.standard_normal(N) * abs(cf - prev_cf) * morph_bow_gain * 0.02
            prev_cf = cf
            x, v = tick(x, v, p, bow)
            energy[t] = np.sqrt(np.mean(v**2))
        return energy

    t = np.arange(ticks)
    # Sweep: cf ramps 0→1 between ticks 300..500, then back 700..750 (fast)
    cf_sweep = np.clip((t - 300) / 200.0, 0, 1) - np.clip((t - 700) / 50.0, 0, 1)
    e_sweep = run(cf_sweep)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 4.4), sharex=True)
    ax1.plot(t / 344.0, cf_sweep, color="#666", linewidth=1.2)
    ax1.set_ylabel("crossfade A→B")
    ax1.grid(alpha=0.3)
    ax2.plot(t / 344.0, e_sweep, color="#a3c", linewidth=1.2)
    ax2.set_ylabel("string kinetic energy (RMS v)")
    ax2.set_xlabel("time (seconds)")
    ax2.grid(alpha=0.3)
    fig.suptitle(
        "Morph-bow: the crossfade's MOTION excites the string\n"
        "slow sweep = gentle bowing; fast snap = hard bow; parked = settles",
        fontsize=9,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"weave_fig5_morph_bow.{ext}", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    fig4()
    fig5()
    print(f"Figures written to {OUTPUT_DIR}")
