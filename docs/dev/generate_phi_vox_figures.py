#!/usr/bin/env python3
"""Generate figures for the PHI_VOX VOSIM documentation (phi-vox-vosim.md).

Reproduces the firmware's synthesis exactly: per fundamental cycle, each formant
branch emits N raised-cosine pulses at the formant rate, scaled by decay^k, then
silence until the cycle wraps.
"""

import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent / "images"
OUTPUT_DIR.mkdir(exist_ok=True)

FS = 44100.0


def vosim_formant(f0, formant_hz, n_pulses, decay, polarity, dur, fs=FS):
    """One VOSIM branch, firmware-equivalent (raised cosine per formant period)."""
    t = np.arange(int(dur * fs)) / fs
    cycle_phase = (t * f0) % 1.0
    samples_into_cycle = cycle_phase / f0 * fs
    formant_cycles = samples_into_cycle * formant_hz / fs
    k = np.floor(formant_cycles).astype(int)
    fphase = formant_cycles % 1.0

    gains = np.zeros(16)
    mag, peak = 1.0, 0.0
    for i in range(min(n_pulses, 16)):
        sign = 1.0 - 2.0 * (i & 1) * polarity
        gains[i] = mag * sign
        peak = max(peak, abs(mag))
        mag *= decay
    gains /= max(peak, 1e-4)

    rc = 0.5 * (1.0 - np.cos(2 * np.pi * fphase))
    return rc * gains[np.clip(k, 0, 15)], t


# ----------------------------------------------------------------------------
# Figure 1: Anatomy of a VOSIM cycle
# ----------------------------------------------------------------------------


def fig1():
    f0 = 110.0
    fig, axes = plt.subplots(3, 1, figsize=(8, 5.4), sharex=True)

    configs = [
        (700.0, 4, 0.65, 0.0, "classic: N=4 pulses, decay 0.65 (formant 700 Hz)"),
        (700.0, 7, 1.15, 0.0, "decay > 1: pulses GROW across the burst (rasp)"),
        (700.0, 5, 0.80, 1.0, "alternating polarity: hollow odd-harmonic variant"),
    ]
    for ax, (fhz, n, b, pol, label) in zip(axes, configs):
        y, t = vosim_formant(f0, fhz, n, b, pol, 2.2 / f0)
        ax.plot(t * 1000, y, linewidth=1.0, color="#36c")
        ax.axhline(0, color="#999", linewidth=0.5)
        for cyc in range(3):
            ax.axvline(cyc / f0 * 1000, color="#c33", linewidth=0.8, linestyle=":")
        ax.set_ylabel("out")
        ax.set_title(label, fontsize=9)
    axes[-1].set_xlabel(
        "time (ms) — red dotted lines mark fundamental cycle starts (110 Hz)"
    )
    fig.suptitle(
        "VOSIM cycle anatomy: pulse burst + silent gap, repeated at the fundamental",
        fontsize=10,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"vox_fig1_anatomy.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 2: The formant in the spectrum (two-branch vowel)
# ----------------------------------------------------------------------------


def fig2():
    f0 = 110.0
    dur = 0.7
    y1, _ = vosim_formant(f0, 730.0, 5, 0.7, 0.0, dur)
    y2, _ = vosim_formant(f0, 1090.0, 4, 0.6, 0.0, dur)
    y = 0.62 * y1 + 0.40 * y2
    y -= np.mean(y)

    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(y), 1 / FS)
    spec_db = 20 * np.log10(spec / spec.max() + 1e-9)

    fig, ax = plt.subplots(figsize=(8, 3.8))
    mask = freqs < 4000
    ax.plot(freqs[mask], spec_db[mask], linewidth=0.9, color="#36c")
    for fmt, name in ((730, "F1"), (1090, "F2")):
        ax.axvline(fmt, color="#c33", linestyle="--", linewidth=1.0)
        ax.text(fmt + 30, -6, f"{name} = {fmt} Hz", fontsize=8, color="#c33")
    ax.set_xlabel("frequency (Hz)")
    ax.set_ylabel("dB")
    ax.set_ylim(-70, 3)
    ax.grid(alpha=0.3)
    ax.set_title(
        "Two VOSIM branches = a vowel: harmonics of 110 Hz shaped by formant humps (/a/-ish)",
        fontsize=9,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"vox_fig2_spectrum.{ext}", dpi=110)
    plt.close(fig)


# ----------------------------------------------------------------------------
# Figure 3: The diphthong — crossfading zone A to zone B glides the formants
# ----------------------------------------------------------------------------


def fig3():
    f0 = 130.0
    steps = 60
    seg = int(FS * 0.03)
    spec_rows = []
    # /a/ (730, 1090) -> /i/ (270, 2290)
    for i in range(steps):
        cf = i / (steps - 1)
        f1hz = 730 * (1 - cf) + 270 * cf
        f2hz = 1090 * (1 - cf) + 2290 * cf
        y1, _ = vosim_formant(f0, f1hz, 5, 0.7, 0.0, seg / FS)
        y2, _ = vosim_formant(f0, f2hz, 4, 0.6, 0.0, seg / FS)
        y = 0.62 * y1 + 0.40 * y2
        y -= np.mean(y)
        spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
        spec_rows.append(20 * np.log10(spec / (spec.max() + 1e-12) + 1e-9))

    spec_img = np.array(spec_rows).T
    freqs = np.fft.rfftfreq(seg, 1 / FS)
    fmask = freqs < 3500

    fig, ax = plt.subplots(figsize=(8, 3.8))
    ax.imshow(
        spec_img[fmask],
        aspect="auto",
        origin="lower",
        cmap="magma",
        extent=[0, 1, 0, 3500],
        vmin=-60,
        vmax=0,
    )
    ax.plot(
        np.linspace(0, 1, steps),
        730 * (1 - np.linspace(0, 1, steps)) + 270 * np.linspace(0, 1, steps),
        "--",
        color="#6cf",
        linewidth=1.0,
    )
    ax.plot(
        np.linspace(0, 1, steps),
        1090 * (1 - np.linspace(0, 1, steps)) + 2290 * np.linspace(0, 1, steps),
        "--",
        color="#6cf",
        linewidth=1.0,
    )
    ax.set_xlabel("wave-index crossfade A → B")
    ax.set_ylabel("frequency (Hz)")
    ax.set_title(
        "The morph is a diphthong: /a/ → /i/ formant glide under the crossfade",
        fontsize=9,
    )
    fig.tight_layout()
    for ext in ("svg", "png"):
        fig.savefig(OUTPUT_DIR / f"vox_fig3_diphthong.{ext}", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    print(f"Figures written to {OUTPUT_DIR}")
