#!/usr/bin/env python3
"""Generate figures for the PHI_GENDY documentation (phi-gendy-stochastic.md).

Reproduces the firmware's double random walk exactly: velocity walks, position
follows, elastic barriers reflect, home pull relaxes.
"""

import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent / "images"
OUTPUT_DIR.mkdir(exist_ok=True)

N = 16
rng = np.random.default_rng(7)


def make_walk(
    steps, step_size, b_hi, b_lo, home, home_pull, vel_cap, kick_at=None, kick_amp=0.0
):
    """Run the firmware's tick for `steps` buffers; return (steps, N) amplitude history."""
    a = np.zeros(N)
    v = np.zeros(N)
    hist = np.zeros((steps, N))
    for t in range(steps):
        kick = kick_amp if (kick_at is not None and kick_at <= t < kick_at + 3) else 0.0
        for i in range(N):
            vel = v[i] + step_size[i] * rng.uniform(-1, 1) + kick * rng.uniform(-1, 1)
            vel = np.clip(vel, -vel_cap, vel_cap)
            amp = a[i] + vel + home_pull * (home[i] - a[i])
            if amp > b_hi[i]:
                amp = 2 * b_hi[i] - amp
                vel = -vel * 0.7
            if amp < b_lo[i]:
                amp = 2 * b_lo[i] - amp
                vel = -vel * 0.7
            a[i] = np.clip(amp, b_lo[i], b_hi[i])
            v[i] = vel
        hist[t] = a - a.mean()
    return hist


nf = np.arange(N) / N
home = 0.7 * np.sin(2 * np.pi * nf) + 0.3 * np.sin(4 * np.pi * nf + 1.7)


# ----------------------------------------------------------------------------
# Figure 1: Waterfall of evolving polygons at three entropy levels
# ----------------------------------------------------------------------------


def fig1():
    fig, axes = plt.subplots(1, 3, figsize=(9.5, 4.2), sharey=True)
    cases = [
        (0.002, "Haze: drift you feel, not hear"),
        (0.03, "Boil: audible writhing"),
        (0.20, "Frenzy: reflections are the sound"),
    ]
    for ax, (step, label) in zip(axes, cases):
        hist = make_walk(
            24,
            np.full(N, step),
            np.full(N, 0.9),
            np.full(N, -0.9),
            home,
            0.05 if step < 0.01 else 0.01,
            0.2,
        )
        xs = np.arange(N + 1)
        for t in range(0, 24, 2):
            poly = np.append(hist[t], hist[t][0])
            ax.plot(xs, poly + t * 0.24, linewidth=0.9, color=plt.cm.viridis(t / 24))
        ax.set_title(label, fontsize=9)
        ax.set_xlabel("breakpoint")
        ax.set_yticks([])
    axes[0].set_ylabel("time (one polygon per 2 ticks, bottom to top)")
    fig.suptitle(
        "The double random walk: one cycle's polygon evolving buffer by buffer",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"gendy_fig1_waterfall.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 2: Spatial landscapes — the cage and entropy vary per node
# ----------------------------------------------------------------------------


def fig2():
    # A pinched, asymmetric cage with an entropy hot-spot
    b_hi = 0.35 + 0.6 * np.abs(np.sin(np.pi * nf * 2 + 0.6))
    b_lo = -0.35 - 0.6 * np.abs(np.sin(np.pi * nf * 2 + 2.1))
    step = 0.004 + 0.05 * np.exp(-((nf - 0.65) ** 2) / 0.01)

    hist = make_walk(400, step, b_hi, b_lo, home * 0.3, 0.02, 0.2)

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(8, 5.0), gridspec_kw={"height_ratios": [1, 2]}
    )
    ax1.plot(nf * N, step * 1000, color="#c33", linewidth=1.4)
    ax1.set_ylabel("step ×1000")
    ax1.set_title("per-node entropy landscape (hot-spot at node ~10)", fontsize=9)
    ax1.grid(alpha=0.3)

    ax2.fill_between(nf * N, b_lo, b_hi, color="#dde5f5", label="elastic cage")
    for t in range(340, 400, 6):
        ax2.plot(
            nf * N,
            hist[t],
            linewidth=0.7,
            color=plt.cm.plasma((t - 340) / 60),
            alpha=0.8,
        )
    ax2.plot(nf * N, b_hi, color="#88a", linewidth=1.0)
    ax2.plot(nf * N, b_lo, color="#88a", linewidth=1.0)
    ax2.set_xlabel("breakpoint")
    ax2.set_ylabel("amplitude")
    ax2.set_title(
        "walkers in the cage: calm where pinched and cold, wild at the hot-spot",
        fontsize=9,
    )
    ax2.legend(fontsize=8, loc="lower left")
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"gendy_fig2_landscape.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 3: The startle — morph motion kicks the walkers, home pull recovers
# ----------------------------------------------------------------------------


def fig3():
    steps = 300
    hist = make_walk(
        steps,
        np.full(N, 0.0015),
        np.full(N, 0.9),
        np.full(N, -0.9),
        home,
        0.06,
        0.2,
        kick_at=120,
        kick_amp=0.15,
    )
    dev = np.sqrt(((hist - (home - home.mean())) ** 2).mean(axis=1))

    fig, ax = plt.subplots(figsize=(8, 3.4))
    t_ms = np.arange(steps) * 128 / 44.1
    ax.plot(t_ms, dev, color="#63a", linewidth=1.1)
    ax.axvspan(
        120 * 128 / 44.1, 123 * 128 / 44.1, color="#fdd", label="morph motion (startle)"
    )
    ax.set_xlabel("time (ms)")
    ax.set_ylabel("RMS deviation from home shape")
    ax.set_title(
        "Startle and recovery: a knob gesture scatters the walkers;\n"
        "the home spring reels them back in",
        fontsize=9,
    )
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"gendy_fig3_startle.{ext}", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    print(f"Figures written to {OUTPUT_DIR}")
