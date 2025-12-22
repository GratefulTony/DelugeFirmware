from dmui.dsl import Menu, Submenu

# Compressor mode selection (Single/Multiband)
mode = Menu(
    "audio_compressor::CompressorModeSelection",
    "compMode",
    ["{name}"],
    "compressor/mode.md",
    name="STRING_FOR_COMPRESSOR_MODE",
)

threshold = Menu(
    "audio_compressor::CompParam",
    "threshold",
    ["{name}", "{title}", "params::UNPATCHED_COMPRESSOR_THRESHOLD", "BAR"],
    "compressor/threshold.md",
    name="STRING_FOR_THRESHOLD",
    title="STRING_FOR_COMP_THRESHOLD_MENU_TITLE",
)

attack = Menu(
    "audio_compressor::Attack",
    "compAttack",
    ["{name}", "{title}"],
    "compressor/attack.md",
    name="STRING_FOR_ATTACK",
)

release = Menu(
    "audio_compressor::Release",
    "compRelease",
    ["{name}", "{title}"],
    "compressor/release.md",
    name="STRING_FOR_RELEASE",
)

ratio = Menu(
    "audio_compressor::Ratio",
    "compRatio",
    ["{name}", "{title}"],
    "compressor/ratio.md",
    name="STRING_FOR_RATIO",
)

hpf = Menu(
    "audio_compressor::SideHPF",
    "compHPF",
    ["{name}", "{title}"],
    "compressor/hpf.md",
    name="STRING_FOR_HPF",
)

blend = Menu(
    "audio_compressor::Blend",
    "compBlend",
    ["{name}", "{title}"],
    "compressor/blend.md",
    name="STRING_FOR_BLEND",
)

# Multiband crossover controls
low_crossover = Menu(
    "audio_compressor::LowCrossover",
    "compLowXover",
    ["{name}"],
    "compressor/low_crossover.md",
    name="STRING_FOR_COMPRESSOR_LOW_CROSSOVER",
)

high_crossover = Menu(
    "audio_compressor::HighCrossover",
    "compHighXover",
    ["{name}"],
    "compressor/high_crossover.md",
    name="STRING_FOR_COMPRESSOR_HIGH_CROSSOVER",
)

menu = Submenu(
    "HorizontalMenu",
    "audioCompMenu",
    ["{name}", "%%CHILDREN%%"],
    "compressor/index.md",
    [
        mode,
        threshold,
        ratio,
        blend,
        attack,
        release,
        hpf,
        low_crossover,
        high_crossover,
    ],
    name="STRING_FOR_COMMUNITY_FEATURE_MASTER_COMPRESSOR",
)
