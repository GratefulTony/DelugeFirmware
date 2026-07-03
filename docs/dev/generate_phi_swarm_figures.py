#!/usr/bin/env python3
"""Generate figures for the PHI_SWARM documentation (phi-swarm-sync.md).

Simulates the firmware's Adler-coupled slave oscillators to render the Arnold
tongue map, injection pulling, and the annealing gesture.
"""

import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent / "images"
OUTPUT_DIR.mkdir(exist_ok=True)

FS = 44100.0


def run_slave(f_master, ratio, k_rel, temp_rel, dur, seed=1):
    """One Adler-coupled slave, firmware-equivalent. Returns slave phase array."""
    rng = np.random.default_rng(seed)
    n = int(dur * FS)
    inc_m = f_master / FS
    inc_s = inc_m * ratio
    k = k_rel * inc_m
    t_amp = temp_rel * inc_m
    theta_m = (np.arange(n) * inc_m) % 1.0
    s = np.zeros(n)
    cur = 0.0
    noise = rng.standard_normal(n) * t_amp
    for i in range(n):
        cur += inc_s + k * np.sin(2 * np.pi * (theta_m[i] - cur)) + noise[i]
        s[i] = cur
    return theta_m, s


# ----------------------------------------------------------------------------
# Figure 1: The Arnold tongue map — the oscillator's actual parameter space
# ----------------------------------------------------------------------------


def fig1():
    f_master = 220.0
    ratios = np.linspace(0.9, 2.1, 241)
    ks = np.linspace(0.0005, 0.06, 60)

    lock_err = np.zeros((len(ks), len(ratios)))
    inc_m = f_master / FS

    for yi, k_rel in enumerate(ks):
        k = k_rel * inc_m
        for xi, r in enumerate(ratios):
            # Winding number of the driven phase (no noise): average slave
            # advance relative to master
            inc_s = r * inc_m
            cur = 0.0
            phi_m = 0.0
            # short settle + measure
            for _ in range(2000):
                cur += inc_s + k * np.sin(2 * np.pi * (phi_m - cur))
                phi_m += inc_m
            start = cur
            phi_start = phi_m
            for _ in range(4000):
                cur += inc_s + k * np.sin(2 * np.pi * (phi_m - cur))
                phi_m += inc_m
            winding = (cur - start) / (phi_m - phi_start)
            # distance from nearest simple rational p/q, q<=4
            best = min(abs(winding - p / q) for q in range(1, 5) for p in range(1, 9))
            lock_err[yi, xi] = best

    fig, ax = plt.subplots(figsize=(8, 4.2))
    im = ax.imshow(
        np.log10(lock_err + 1e-6),
        aspect="auto",
        origin="lower",
        extent=[ratios[0], ratios[-1], ks[0], ks[-1]],
        cmap="inferno_r",
    )
    ax.set_xlabel("slave detune ratio (slave freq / master freq)")
    ax.set_ylabel("coupling K (fraction of master increment)")
    ax.set_title(
        "Arnold tongues, measured from the firmware's Adler update:\n"
        "bright = locked to a rational ratio (consonant), dark = quasiperiodic",
        fontsize=9,
    )
    fig.colorbar(im, label="log10 distance from nearest p/q")
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"swarm_fig1_tongues.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 2: Injection pulling — the beat that hesitates and snaps
# ----------------------------------------------------------------------------


def fig2():
    f_master = 220.0
    dur = 0.6
    fig, axes = plt.subplots(3, 1, figsize=(8, 5.2), sharex=True)
    cases = [
        (1.002, 0.008, "inside the tongue: locked (beat frequency -> 0)"),
        (1.010, 0.008, "near the edge: injection pulling - slow, asymmetric beat"),
        (1.060, 0.008, "outside: free quasiperiodic beating"),
    ]
    for ax, (ratio, k, label) in zip(axes, cases):
        theta_m, s = run_slave(f_master, ratio, k, 0.0, dur)
        # Phase difference (the beat): wrapped
        diff = (s - theta_m) % 1.0
        t = np.arange(len(diff)) / FS
        ax.plot(t, diff, linewidth=0.8, color="#36c")
        ax.set_ylabel("phase diff")
        ax.set_title(label, fontsize=9)
        ax.set_ylim(0, 1)
    axes[-1].set_xlabel(
        "time (s) — flat line = locked; sawtooth = beating; "
        "slow bendy sawtooth = pulling"
    )
    fig.suptitle("Injection locking and pulling (Adler 1946)", fontsize=10)
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"swarm_fig2_pulling.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 3: Annealing — morph motion heats the swarm, parking cools it
# ----------------------------------------------------------------------------


def fig3():
    f_master = 220.0
    dur = 2.2
    n = int(dur * FS)
    inc_m = f_master / FS
    k = 0.012 * inc_m
    inc_s = 1.004 * inc_m
    rng = np.random.default_rng(3)

    # Temperature profile: cold, then a "morph sweep" heats it, then cools
    t = np.arange(n) / FS
    heat = np.where((t > 0.7) & (t < 1.1), 0.05, 0.0)
    # Exponential cooling after the sweep stops
    temp = np.zeros(n)
    cur_T = 0.0
    for i in range(n):
        cur_T = max(heat[i], cur_T * 0.99993)
        temp[i] = cur_T

    theta_m = (np.arange(n) * inc_m) % 1.0
    s = np.zeros(n)
    cur = 0.0
    noise = rng.standard_normal(n)
    for i in range(n):
        cur += (
            inc_s
            + k * np.sin(2 * np.pi * (theta_m[i] - cur))
            + noise[i] * temp[i] * inc_m
        )
        s[i] = cur

    diff = (s - theta_m) % 1.0
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 4.4), sharex=True)
    ax1.plot(t, temp, color="#c33", linewidth=1.2)
    ax1.set_ylabel("temperature")
    ax1.grid(alpha=0.3)
    ax2.plot(t, diff, linewidth=0.7, color="#63a")
    ax2.set_ylabel("slave phase diff")
    ax2.set_xlabel("time (s)")
    ax2.grid(alpha=0.3)
    fig.suptitle(
        "Annealing: crossfade motion heats the swarm out of lock;\n"
        "parked, it cools and audibly re-crystallizes",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"swarm_fig3_annealing.{ext}", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    print(f"Figures written to {OUTPUT_DIR}")
