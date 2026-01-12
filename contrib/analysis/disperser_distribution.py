#!/usr/bin/env python3
"""
Visualize disperser stage density as a function of frequency.
"""

import numpy as np
import matplotlib.pyplot as plt
from scipy.stats import gaussian_kde


def compute_stage_frequencies(
    num_stages: int = 16,
    center_hz: float = 1000.0,
    spread_octaves: float = 2.0,
    spread_curve: float = 0.5,
    bimodal_separation: float = 0.0,
):
    """Compute frequency for each stage given disperser parameters."""
    curve_clamped = np.clip(spread_curve, 0.0, 1.0)
    exponent = 5.0 ** (1.0 - curve_clamped * 2.0)

    is_bimodal = bimodal_separation > 0.02

    if is_bimodal:
        half_sep = bimodal_separation * 0.5
        mode_a_hz = center_hz / (2.0**half_sep)
        mode_b_hz = center_hz * (2.0**half_sep)
    else:
        mode_a_hz = center_hz
        mode_b_hz = center_hz

    frequencies = []

    for i in range(num_stages):
        t = i / max(1.0, num_stages - 1)

        if is_bimodal:
            is_mode_b = i % 2 == 1
            mode_center = mode_b_hz if is_mode_b else mode_a_hz
            stages_in_mode = (num_stages + (0 if is_mode_b else 1)) // 2
            index_in_mode = i // 2
            t_local = index_in_mode / max(1.0, stages_in_mode - 1)
            curved = t_local**exponent
            local_position = -curved if is_mode_b else curved
            local_spread = spread_octaves * 0.5
            stage_hz = mode_center * (2.0 ** (local_position * local_spread))
        else:
            curved = t**exponent
            stage_position = curved * 2.0 - 1.0
            stage_hz = center_hz * (2.0 ** (stage_position * spread_octaves))

        stage_hz = np.clip(stage_hz, 20.0, 16000.0)
        frequencies.append(stage_hz)

    return np.array(frequencies)


def plot_density_vs_freq():
    """Plot stage density as a function of frequency for various curve values."""
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))

    num_stages = 32
    center_hz = 1000.0

    # Use log-frequency for KDE (more natural for audio)
    freq_range = np.linspace(np.log10(50), np.log10(16000), 500)
    freq_hz = 10**freq_range

    # Top-left: varying curve at fixed spread
    ax = axes[0, 0]
    spread = 2.0
    curves = [0.0, 0.25, 0.5, 0.75, 1.0]
    for curve in curves:
        freqs = compute_stage_frequencies(num_stages, center_hz, spread, curve)
        log_freqs = np.log10(freqs)
        kde = gaussian_kde(log_freqs, bw_method=0.15)
        density = kde(freq_range)
        ax.plot(freq_hz / 1000, density, label=f"curve={curve:.2f}")

    ax.axvline(center_hz / 1000, color="gray", linestyle="--", alpha=0.5)
    ax.set_xscale("log")
    ax.set_xlabel("Frequency (kHz)")
    ax.set_ylabel("Stage Density")
    ax.set_title(f"Curve Effect (spread={spread} oct)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Top-right: varying spread at fixed curve
    ax = axes[0, 1]
    curve = 0.5
    spreads = [0.5, 1.0, 2.0, 3.0, 4.0]
    for spread in spreads:
        freqs = compute_stage_frequencies(num_stages, center_hz, spread, curve)
        log_freqs = np.log10(freqs)
        kde = gaussian_kde(log_freqs, bw_method=0.15)
        density = kde(freq_range)
        ax.plot(freq_hz / 1000, density, label=f"spread={spread} oct")

    ax.axvline(center_hz / 1000, color="gray", linestyle="--", alpha=0.5)
    ax.set_xscale("log")
    ax.set_xlabel("Frequency (kHz)")
    ax.set_ylabel("Stage Density")
    ax.set_title(f"Spread Effect (curve={curve})")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Bottom-left: bimodal separation
    ax = axes[1, 0]
    spread = 1.0
    curve = 0.5
    separations = [0.0, 0.25, 0.5, 0.75, 1.0]
    for sep in separations:
        freqs = compute_stage_frequencies(num_stages, center_hz, spread, curve, sep)
        log_freqs = np.log10(freqs)
        kde = gaussian_kde(log_freqs, bw_method=0.15)
        density = kde(freq_range)
        label = "single" if sep < 0.02 else f"sep={sep} oct"
        ax.plot(freq_hz / 1000, density, label=label)

    ax.axvline(center_hz / 1000, color="gray", linestyle="--", alpha=0.5)
    ax.set_xscale("log")
    ax.set_xlabel("Frequency (kHz)")
    ax.set_ylabel("Stage Density")
    ax.set_title("Bimodal Separation Effect")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Bottom-right: bimodal with different curves
    ax = axes[1, 1]
    spread = 1.0
    bimodal_sep = 0.5
    curves = [0.0, 0.25, 0.5, 0.75, 1.0]
    for curve in curves:
        freqs = compute_stage_frequencies(
            num_stages, center_hz, spread, curve, bimodal_sep
        )
        log_freqs = np.log10(freqs)
        kde = gaussian_kde(log_freqs, bw_method=0.15)
        density = kde(freq_range)
        ax.plot(freq_hz / 1000, density, label=f"curve={curve:.2f}")

    # Show mode centers
    half_sep = bimodal_sep * 0.5
    mode_a = center_hz / (2.0**half_sep) / 1000
    mode_b = center_hz * (2.0**half_sep) / 1000
    ax.axvline(mode_a, color="blue", linestyle=":", alpha=0.5)
    ax.axvline(mode_b, color="red", linestyle=":", alpha=0.5)

    ax.set_xscale("log")
    ax.set_xlabel("Frequency (kHz)")
    ax.set_ylabel("Stage Density")
    ax.set_title(f"Bimodal Curve Effect (sep={bimodal_sep} oct)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig("disperser_density.png", dpi=150)
    plt.show()


if __name__ == "__main__":
    plot_density_vs_freq()
