"""Images staged in assets/launcher by every desktop and Android build."""

SIDE_ART = (
    "menu-sides.png",
    "shop-sides.png",
    "save-native-sides.png",
    "language-native-sides.png",
    "title-native-sides.png",
    "shop-native-sides.png",
    "forest-native-sides.png",
    # Flat (4:3) art: drafts made with tools/flat_synth.py from the approved 16:9 art, pending approval
    "language-flat.png",
    "title-flat.png",
    "save-flat.png",
    "forest-flat.png",
    "shop-flat.png",
)

LAUNCHER_ART = ("cover.png", *SIDE_ART)
