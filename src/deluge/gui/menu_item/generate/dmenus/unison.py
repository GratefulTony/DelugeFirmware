from dmui.dsl import Menu, Submenu

count = Menu(
    "unison::CountToStereoSpread",
    "numUnisonMenu",
    ["{name}", "{title}"],
    "oscillator/unison/count.md",
    name="STRING_FOR_UNISON_NUMBER",
    title="STRING_FOR_UNISON_NUMBER_MENU_TITLE",
)

detune = Menu(
    "unison::Detune",
    "unisonDetuneMenu",
    ["{name}", "{title}"],
    "oscillator/unison/detune.md",
    name="STRING_FOR_UNISON_DETUNE",
    title="STRING_FOR_UNISON_DETUNE_MENU_TITLE",
)

stereo_spread = Menu(
    "unison::StereoSpread",
    "unison::stereoSpreadMenu",
    ["{name}", "{title}"],
    "oscillator/unison/stereo_spread.md",
    name="STRING_FOR_UNISON_STEREO_SPREAD",
    title="STRING_FOR_UNISON_STEREO_SPREAD_MENU_TITLE",
)

index_curve = Menu(
    "unison::IndexCurve",
    "unisonIndexCurveMenu",
    ["{name}", "{title}"],
    "oscillator/unison/index_curve.md",
    name="STRING_FOR_UNISON_INDEX_CURVE",
    title="STRING_FOR_UNISON_INDEX_CURVE_MENU_TITLE",
)

index_shape = Menu(
    "unison::IndexShape",
    "unisonIndexShapeMenu",
    ["{name}", "{title}"],
    "oscillator/unison/index_shape.md",
    name="STRING_FOR_UNISON_INDEX_SHAPE",
    title="STRING_FOR_UNISON_INDEX_SHAPE_MENU_TITLE",
)

index_mapping = Menu(
    "unison::IndexMapping",
    "unisonIndexMappingMenu",
    ["{name}", "{title}"],
    "oscillator/unison/index_mapping.md",
    name="STRING_FOR_UNISON_INDEX_MAPPING",
    title="STRING_FOR_UNISON_INDEX_MAPPING_MENU_TITLE",
)

menu = Submenu(
    "HorizontalMenu",
    "unisonMenu",
    ["{name}", "%%CHILDREN%%", "HorizontalMenu::Layout::FIXED"],
    "oscillator/unison/index.md",
    [
        count,
        detune,
        stereo_spread,
        index_curve,
        index_shape,
        index_mapping,
    ],
    name="STRING_FOR_UNISON",
)
