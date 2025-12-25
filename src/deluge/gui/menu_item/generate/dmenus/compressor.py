from dmui.dsl import Menu, Submenu

# === Single-band compressor items ===
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

# === Main Compressor Menu (single-band only) ===
menu = Submenu(
    "HorizontalMenu",
    "audioCompMenu",
    ["{name}", "%%CHILDREN%%"],
    "compressor/index.md",
    [
        threshold,
        ratio,
        blend,
        attack,
        release,
        hpf,
    ],
    name="STRING_FOR_COMMUNITY_FEATURE_MASTER_COMPRESSOR",
)

# === DOTT (Multiband Compressor) items ===

# Mode zone - first item, controls on/off
dott_mode = Menu(
    "audio_compressor::ModeZone",
    "dottMode",
    ["{name}"],
    "compressor/ratio.md",  # Reuse existing doc
    name="STRING_FOR_DOTT_MODE",
)

linked_threshold = Menu(
    "audio_compressor::LinkedThreshold",
    "mbLinkedThreshold",
    ["{name}"],
    "compressor/threshold.md",
    name="STRING_FOR_THRESHOLD",
)

linked_ratio = Menu(
    "audio_compressor::LinkedRatio",
    "mbLinkedRatio",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_RATIO",
)

linked_attack = Menu(
    "audio_compressor::LinkedAttack",
    "mbLinkedAttack",
    ["{name}"],
    "compressor/attack.md",
    name="STRING_FOR_ATTACK",
)

linked_release = Menu(
    "audio_compressor::LinkedRelease",
    "mbLinkedRelease",
    ["{name}"],
    "compressor/release.md",
    name="STRING_FOR_RELEASE",
)

character = Menu(
    "audio_compressor::Character",
    "mbCharacter",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_CHARACTER",
)

up_down_skew = Menu(
    "audio_compressor::UpDownSkew",
    "mbUpDownSkew",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_UP_DOWN_SKEW",
)

output_gain = Menu(
    "audio_compressor::OutputGain",
    "mbOutputGain",
    ["{name}"],
    "compressor/blend.md",
    name="STRING_FOR_COMPRESSOR_OUTPUT_GAIN",
)

vibe = Menu(
    "audio_compressor::Vibe",
    "mbVibe",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_VIBE",
)

# === Low Band items ===
low_threshold = Menu(
    "audio_compressor::BandThreshold<0>",
    "mbLowThreshold",
    ["{name}"],
    "compressor/threshold.md",
    name="STRING_FOR_COMPRESSOR_LOW_THRESHOLD",
)

low_ratio = Menu(
    "audio_compressor::BandRatio<0>",
    "mbLowRatio",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_LOW_RATIO",
)

low_bw = Menu(
    "audio_compressor::BandBandwidth<0>",
    "mbLowBandwidth",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_LOW_BW",
)

low_output_level = Menu(
    "audio_compressor::BandOutputLevel<0>",
    "mbLowOutputLevel",
    ["{name}"],
    "compressor/blend.md",
    name="STRING_FOR_COMPRESSOR_LOW_LEVEL",
)

# === Mid Band items ===
mid_threshold = Menu(
    "audio_compressor::BandThreshold<1>",
    "mbMidThreshold",
    ["{name}"],
    "compressor/threshold.md",
    name="STRING_FOR_COMPRESSOR_MID_THRESHOLD",
)

mid_ratio = Menu(
    "audio_compressor::BandRatio<1>",
    "mbMidRatio",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_MID_RATIO",
)

mid_bw = Menu(
    "audio_compressor::BandBandwidth<1>",
    "mbMidBandwidth",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_MID_BW",
)

mid_output_level = Menu(
    "audio_compressor::BandOutputLevel<1>",
    "mbMidOutputLevel",
    ["{name}"],
    "compressor/blend.md",
    name="STRING_FOR_COMPRESSOR_MID_LEVEL",
)

# === High Band items ===
high_threshold = Menu(
    "audio_compressor::BandThreshold<2>",
    "mbHighThreshold",
    ["{name}"],
    "compressor/threshold.md",
    name="STRING_FOR_COMPRESSOR_HIGH_THRESHOLD",
)

high_ratio = Menu(
    "audio_compressor::BandRatio<2>",
    "mbHighRatio",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_HIGH_RATIO",
)

high_bw = Menu(
    "audio_compressor::BandBandwidth<2>",
    "mbHighBandwidth",
    ["{name}"],
    "compressor/ratio.md",
    name="STRING_FOR_COMPRESSOR_HIGH_BW",
)

high_output_level = Menu(
    "audio_compressor::BandOutputLevel<2>",
    "mbHighOutputLevel",
    ["{name}"],
    "compressor/blend.md",
    name="STRING_FOR_COMPRESSOR_HIGH_LEVEL",
)

# === Crossover items ===
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

mb_blend = Menu(
    "audio_compressor::MultibandBlend",
    "mbBlend",
    ["{name}"],
    "compressor/blend.md",
    name="STRING_FOR_BLEND",
)

# === DOTT Menu (separate FX entry) ===
# Uses specialized CompressorHorizontalMenu to render GR meter in header
dott_menu = Submenu(
    "submenu::CompressorHorizontalMenu",
    "dottMenu",
    ["{name}", "%%CHILDREN%%"],
    "compressor/index.md",
    [
        # Mode zone - first item (on/off)
        dott_mode,
        # Page 1 (Compression): threshold, ratio, up/down skew
        linked_threshold,
        linked_ratio,
        up_down_skew,
        # Page 2 (Timing/Dynamics): attack, release, character, vibe
        linked_attack,
        linked_release,
        character,
        vibe,
        # Low band page - 4 items
        low_threshold,
        low_ratio,
        low_bw,
        low_output_level,
        # Mid band page - 4 items
        mid_threshold,
        mid_ratio,
        mid_bw,
        mid_output_level,
        # High band page - 4 items
        high_threshold,
        high_ratio,
        high_bw,
        high_output_level,
        # Crossover/Output page - 4 items
        low_crossover,
        high_crossover,
        output_gain,
        mb_blend,
    ],
    name="STRING_FOR_DOTT",
)
