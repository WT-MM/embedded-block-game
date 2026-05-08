from __future__ import annotations

from pathlib import Path
import sys

# Pillow is imported lazily inside write_preview() so the .mif emission
# path (which has no extra dependencies) still works on hosts that don't
# have PIL installed.

TILE_SIZE = 16
TILE_COUNT = 128
TEXELS_PER_TILE = TILE_SIZE * TILE_SIZE
ATLAS_BYTES = TILE_COUNT * TEXELS_PER_TILE

TEX_TILE_GRASS_TOP = 0
TEX_TILE_GRASS_SIDE = 1
TEX_TILE_DIRT = 2
TEX_TILE_STONE = 3
TEX_TILE_WOOD_SIDE = 4
TEX_TILE_WOOD_TOP = 5
TEX_TILE_GLASS = 6
TEX_TILE_LAMP = 7
TEX_TILE_LEAVES = 8
TEX_TILE_WOOD_PLANK = 9
TEX_TILE_GRASS_TOP_MIP1 = 16
TEX_TILE_GRASS_SIDE_MIP1 = 17
TEX_TILE_DIRT_MIP1 = 18
TEX_TILE_STONE_MIP1 = 19
TEX_TILE_WOOD_SIDE_MIP1 = 20
TEX_TILE_WOOD_TOP_MIP1 = 21
TEX_TILE_GLASS_MIP1 = 22
TEX_TILE_LAMP_MIP1 = 23
TEX_TILE_GRASS_TOP_MIP2 = 24
TEX_TILE_GRASS_SIDE_MIP2 = 25
TEX_TILE_DIRT_MIP2 = 26
TEX_TILE_STONE_MIP2 = 27
TEX_TILE_WOOD_SIDE_MIP2 = 28
TEX_TILE_WOOD_TOP_MIP2 = 29
TEX_TILE_GLASS_MIP2 = 30
TEX_TILE_LAMP_MIP2 = 31
TEX_TILE_LEAVES_MIP1 = 32
TEX_TILE_LEAVES_MIP2 = 33
TEX_TILE_WOOD_PLANK_MIP1 = 34
TEX_TILE_WOOD_PLANK_MIP2 = 35
TEX_TILE_SKY = 48
TEX_TILE_CLOUD = 49
TEX_TILE_SUN = 50
TEX_TILE_MOON = 51
TEX_TILE_STARS = 52
TEX_TILE_CROSSHAIR = 63

PAL_TRANSPARENT = 0
PAL_GRASS_TOP = 1
PAL_DIRT = 2
PAL_WOOD = 3
PAL_STONE = 4
PAL_WHITE = 5
PAL_GRASS_SIDE = 9
PAL_WOOD_TOP = 11
PAL_GRASS_DARK = 17
PAL_GRASS_LIGHT = 18
PAL_DIRT_DARK = 19
PAL_DIRT_LIGHT = 20
PAL_WOOD_GRAIN = 21
PAL_WOOD_DARK = 22
PAL_STONE_DARK = 23
PAL_STONE_LIGHT = 24
PAL_SKY_HIGH = 25
PAL_SKY_MID = 26
PAL_SKY_HORIZON = 27
PAL_CLOUD = 28
PAL_CLOUD_SHADOW = 29
PAL_SUN_CORE = 30
PAL_SUN_GLOW = 31
PAL_MOON = 32
PAL_MOON_SHADOW = 33
PAL_STAR = 34
PAL_GLASS = 35
PAL_GLASS_EDGE = 36
PAL_GLASS_HIGHLIGHT = 37
PAL_LAMP_GLOW = 38
PAL_LAMP_FRAME = 39


# RGB values for each palette index. Mirrors the table in
# `sw/renderer.c::upload_default_palette()` and is used only by
# write_preview() to rasterise the atlas into a PNG. Any palette index
# not listed here renders as solid magenta in the preview so missed
# updates are impossible to miss visually.
PREVIEW_PALETTE: dict[int, tuple[int, int, int]] = {
    0:  (0x10, 0x10, 0x18),  # background / transparent
    1:  (0x74, 0xb3, 0x44),  # grass top
    2:  (0x86, 0x5f, 0x3c),  # dirt side
    3:  (0x6f, 0x57, 0x37),  # wood side
    4:  (0x7c, 0x7c, 0x7c),  # stone side
    5:  (0xff, 0xff, 0xff),  # debug white
    6:  (0xff, 0x40, 0x40),  # debug red
    7:  (0x40, 0xa0, 0xff),  # debug blue
    8:  (0xff, 0xd0, 0x40),  # debug yellow
    9:  (0x66, 0x95, 0x39),  # grass side
    10: (0x6a, 0x4a, 0x2c),  # grass bottom / dark dirt
    11: (0x9d, 0x7b, 0x4d),  # wood top
    12: (0x53, 0x38, 0x23),  # wood bottom
    13: (0x98, 0x98, 0x98),  # stone top
    14: (0x5c, 0x5c, 0x5c),  # stone bottom
    15: (0xa7, 0x79, 0x52),  # dirt top
    16: (0x59, 0x41, 0x2a),  # dirt bottom
    17: (0x4e, 0x7a, 0x2b),  # grass dark
    18: (0x8b, 0xc7, 0x5a),  # grass highlight
    19: (0x6a, 0x49, 0x2f),  # dirt dark
    20: (0xa7, 0x7c, 0x53),  # dirt light
    21: (0x88, 0x6a, 0x44),  # wood grain
    22: (0x50, 0x3b, 0x24),  # wood bark dark
    23: (0x63, 0x63, 0x63),  # stone dark
    24: (0x9a, 0x9a, 0x9a),  # stone light
    25: (0x78, 0xb4, 0xf0),  # sky high
    26: (0xb0, 0xd8, 0xff),  # sky mid
    27: (0xe2, 0xef, 0xff),  # sky horizon
    28: (0xf5, 0xfa, 0xff),  # cloud
    29: (0xcb, 0xd8, 0xec),  # cloud shadow
    30: (0xff, 0xee, 0xaa),  # sun core
    31: (0xff, 0xbb, 0x55),  # sun glow
    32: (0xe7, 0xeb, 0xf8),  # moon
    33: (0x9c, 0xa4, 0xc0),  # moon shadow
    34: (0xff, 0xff, 0xff),  # stars
    35: (0xb8, 0xe4, 0xff),  # glass body
    36: (0x5e, 0x7c, 0x98),  # glass edge / frame
    37: (0xff, 0xff, 0xff),  # glass highlight
    38: (0xff, 0xd7, 0x79),  # lamp glow
    39: (0x6d, 0x53, 0x30),  # lamp frame
}

# Tiles shown in the preview, one column per block type, one row per LOD.
# Columns are chosen so the preview matches what a player actually sees on
# a real block face; add/remove entries as the atlas grows.
PREVIEW_COLUMNS: list[tuple[str, int, int, int]] = [
    # (label, base_tile, mip1_tile, mip2_tile)
    ("grass_top",  TEX_TILE_GRASS_TOP,  TEX_TILE_GRASS_TOP_MIP1,  TEX_TILE_GRASS_TOP_MIP2),
    ("grass_side", TEX_TILE_GRASS_SIDE, TEX_TILE_GRASS_SIDE_MIP1, TEX_TILE_GRASS_SIDE_MIP2),
    ("dirt",       TEX_TILE_DIRT,       TEX_TILE_DIRT_MIP1,       TEX_TILE_DIRT_MIP2),
    ("stone",      TEX_TILE_STONE,      TEX_TILE_STONE_MIP1,      TEX_TILE_STONE_MIP2),
    ("wood_side",  TEX_TILE_WOOD_SIDE,  TEX_TILE_WOOD_SIDE_MIP1,  TEX_TILE_WOOD_SIDE_MIP2),
    ("wood_top",   TEX_TILE_WOOD_TOP,   TEX_TILE_WOOD_TOP_MIP1,   TEX_TILE_WOOD_TOP_MIP2),
    ("glass",      TEX_TILE_GLASS,      TEX_TILE_GLASS_MIP1,      TEX_TILE_GLASS_MIP2),
    ("lamp",       TEX_TILE_LAMP,       TEX_TILE_LAMP_MIP1,       TEX_TILE_LAMP_MIP2),
    ("leaves",     TEX_TILE_LEAVES,     TEX_TILE_LEAVES_MIP1,     TEX_TILE_LEAVES_MIP2),
    ("wood_plank", TEX_TILE_WOOD_PLANK, TEX_TILE_WOOD_PLANK_MIP1, TEX_TILE_WOOD_PLANK_MIP2),
]



MIP1_TILES = {
    TEX_TILE_GRASS_TOP_MIP1: TEX_TILE_GRASS_TOP,
    TEX_TILE_GRASS_SIDE_MIP1: TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT_MIP1: TEX_TILE_DIRT,
    TEX_TILE_STONE_MIP1: TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE_MIP1: TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP_MIP1: TEX_TILE_WOOD_TOP,
    TEX_TILE_GLASS_MIP1: TEX_TILE_GLASS,
    TEX_TILE_LAMP_MIP1: TEX_TILE_LAMP,
    TEX_TILE_LEAVES_MIP1: TEX_TILE_LEAVES,
    TEX_TILE_WOOD_PLANK_MIP1: TEX_TILE_WOOD_PLANK,
}

MIP2_TILES = {
    TEX_TILE_GRASS_TOP_MIP2: TEX_TILE_GRASS_TOP,
    TEX_TILE_GRASS_SIDE_MIP2: TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT_MIP2: TEX_TILE_DIRT,
    TEX_TILE_STONE_MIP2: TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE_MIP2: TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP_MIP2: TEX_TILE_WOOD_TOP,
    TEX_TILE_GLASS_MIP2: TEX_TILE_GLASS,
    TEX_TILE_LAMP_MIP2: TEX_TILE_LAMP,
    TEX_TILE_LEAVES_MIP2: TEX_TILE_LEAVES,
    TEX_TILE_WOOD_PLANK_MIP2: TEX_TILE_WOOD_PLANK,
}

GRASS_TOP_ROWS = [
    "ggglggdgggglggdg",
    "gdggggglgdgggggg",
    "ggggdgggglgggdgg",
    "lggggggdgggglggg",
    "ggdggglggggdgggg",
    "gggglggggdgggglg",
    "dgggggglgggggdgg",
    "gglggdgggglggggg",
    "gggggglggdgggglg",
    "lggdgggggglggggd",
    "gggglggdgggggglg",
    "dgggggglggggdggg",
    "gglggggdggglgggg",
    "ggggdgggglggggdg",
    "lggggglggdgggggg",
    "ggdgggggglggdggl",
]

DIRT_ROWS = [
    "bbdbbbbblbbbdbbb",
    "bbbbdbbbbbbblbbb",
    "dbbbbbbbdbbbbbbl",
    "bbbblbbbbbbbdbbb",
    "bbbbbdbbbblbbbbb",
    "lbbbbbbbbdbbbbdb",
    "bbbbdbbbbbblbbbb",
    "bbbblbbbdbbbbbbb",
    "dbbbbbblbbbbdbbb",
    "bbbbbbdbbbbbblbb",
    "bbdbbbbbblbbbbbb",
    "bbbblbbbbbbbdbbb",
    "bbbdbbbbblbbbbdb",
    "lbbbbbbdbbbbbbbb",
    "bbbbdbbbbbblbbbl",
    "bbbbbblbbbdbbbbb",
]

GRASS_SIDE_ROWS = [
    "ggglggdgggglggdg",
    "gdggggglgdgggggg",
    "ggggdgggglgggdgg",
    "bdbbbbbblbbbdbbb",
    "bbbbdbbbbbbblbbb",
    "dbbbbbbbdbbbbbbl",
    "bbbblbbbbbbbdbbb",
    "bbbbbdbbbblbbbbb",
    "lbbbbbbbbdbbbbdb",
    "bbbbdbbbbbblbbbb",
    "bbbblbbbdbbbbbbb",
    "dbbbbbblbbbbdbbb",
    "bbbbbbdbbbbbblbb",
    "bbdbbbbbblbbbbbb",
    "bbbblbbbbbbbdbbb",
    "bbbdbbbbblbbbbdb",
]

STONE_ROWS = [
    "ssssdssslssssssd",
    "dsssssssdssslsss",
    "ssslssssdsssssss",
    "sssssdssssslsssd",
    "lsssssssssdsssss",
    "sssdssslssssdsss",
    "ssssssssslsssssd",
    "dssslsssssssssss",
    "ssssssdssslsssss",
    "ssslsssssdssssls",
    "ssssssslsssssdss",
    "dssssssssdssslss",
    "ssssldsssssssssd",
    "sssdssssslssssss",
    "sssssssdssssslss",
    "lssssdsssssssdss",
]

WOOD_SIDE_ROWS = [
    "dbgbbggbbgbbggbd",
    "dbgbbbgbbggbbbdd",
    "dggbbbgbbgbbggbd",
    "dbgbbggbbbgbbbgd",
    "dggbbbgbbgbbggbd",
    "dbgbbbgbbggbbbdd",
    "dbgbbggbbgbbggbd",
    "dggbbbgbbgbbggbd",
    "dbgbbggbbbgbbbgd",
    "dbgbbbgbbggbbbdd",
    "dggbbbgbbgbbggbd",
    "dbgbbggbbgbbggbd",
    "dbgbbbgbbggbbbdd",
    "dggbbbgbbgbbggbd",
    "dbgbbggbbbgbbbgd",
    "dggbbbgbbgbbggbd",
]


LEAVES_ROWS = [
    "tggltggdggtlgttg",
    "ggdggtgglggdggtg",
    "tgglggdgtgglggdt",
    "ggtgglggdggtgglg",
    "dggtgglggdgtgglt",
    "gglgdtggtgglggdg",
    "tggdgglggtggdggt",
    "ggtggdgglggtggld",
    "lggtgglgdtgglggt",
    "ggdggtgglggdggtl",
    "tgglggdgtgglggdt",
    "ggtgglggdggtgglg",
    "dggtgglggdgtgglt",
    "gglgdtggtgglggdg",
    "tggdgglggtggdggt",
    "ggtggdgglggtgglt",
]

SOURCE_TEXTURE_FILES: dict[int, str] = {
    TEX_TILE_GRASS_TOP: "grass_block_top.png",
    TEX_TILE_GRASS_SIDE: "grass_block_side.png",
    TEX_TILE_DIRT: "dirt.png",
    TEX_TILE_STONE: "stone.png",
    TEX_TILE_WOOD_SIDE: "oak_log.png",
    TEX_TILE_WOOD_TOP: "oak_log_top.png",
    TEX_TILE_WOOD_PLANK: "oak_planks.png",
    TEX_TILE_GLASS: "glass.png",
    TEX_TILE_LEAVES: "oak_leaves.png",
}

# Restrict quantization per tile so imported textures remain faithful while
# still mapping to the renderer's fixed hardware palette.
SOURCE_TILE_ALLOWED_PALETTE: dict[int, tuple[int, ...]] = {
    TEX_TILE_GRASS_TOP: (1, 17, 18),
    TEX_TILE_GRASS_SIDE: (9, 17, 18, 2, 19, 20),
    TEX_TILE_DIRT: (2, 19, 20),
    TEX_TILE_STONE: (4, 23, 24),
    TEX_TILE_WOOD_SIDE: (11, 3, 21, 22),
    TEX_TILE_WOOD_TOP: (11, 3, 21, 22),
    TEX_TILE_WOOD_PLANK: (11, 3, 21, 22),
    TEX_TILE_GLASS: (0, 35, 36, 37),
    TEX_TILE_LEAVES: (0, 9, 17, 18, 2, 19),
}

# Vanilla grass/leaves textures are grayscale masks intended for biome tinting.
# Apply a fixed lush-green tint in this project since we do not have biome maps.
SOURCE_TILE_TINT: dict[int, tuple[int, int, int]] = {
    TEX_TILE_GRASS_TOP: (121, 192, 90),
    TEX_TILE_GRASS_SIDE: (121, 192, 90),
    TEX_TILE_LEAVES: (106, 170, 72),
}


def pattern_texel(rows: list[str], mapping: dict[str, int], x: int, y: int) -> int:
    return mapping[rows[y][x]]


def noise(x: int, y: int, seed: int) -> int:
    value = (x * 97) ^ (y * 57) ^ (seed * 131)
    value = (value * 1103515245 + 12345) & 0x7FFFFFFF
    return value & 0xFF


_SOURCE_TILE_CACHE: dict[int, list[tuple[int, int, int, int]]] = {}


def _quantize_to_palette(tile: int, rgba: tuple[int, int, int, int], x: int, y: int) -> int:
    r, g, b, a = rgba
    if a < 96:
        return PAL_TRANSPARENT

    if tile == TEX_TILE_GRASS_TOP:
        # Darker, tighter 2-tone mapping: avoids bright highlights and keeps
        # the contrast range closer to vanilla grass top in this palette.
        bayer4 = (
            (0, 8, 2, 10),
            (12, 4, 14, 6),
            (3, 11, 1, 9),
            (15, 7, 13, 5),
        )
        gray = (r + g + b) // 3
        dither = bayer4[y & 3][x & 3] - 8
        gray = max(0, min(255, gray + dither * 2))
        if gray < 148:
            return PAL_GRASS_DARK
        return PAL_GRASS_SIDE

    if tile == TEX_TILE_GLASS:
        # Vanilla glass has very low alpha body with bright streaks. Generic
        # nearest-color quantization makes it blotchy in our tiny palette, so
        # classify explicitly by opacity and brightness.
        luma = (r * 30 + g * 59 + b * 11) // 100
        if a < 140:
            return PAL_TRANSPARENT
        if luma >= 200:
            return PAL_GLASS_HIGHLIGHT
        if a >= 220 or luma <= 130:
            return PAL_GLASS_EDGE
        return PAL_GLASS

    tint = SOURCE_TILE_TINT.get(tile)
    if tint is not None:
        apply_tint = True

        if tile == TEX_TILE_GRASS_SIDE:
            # Keep dirt pixels brown; tint only the gray/neutral grass mask
            # regions from vanilla side textures.
            max_c = max(r, g, b)
            min_c = min(r, g, b)
            is_neutral_mask = (max_c - min_c) <= 26
            apply_tint = is_neutral_mask

        if apply_tint:
            tr, tg, tb = tint
            r = (r * tr) // 255
            g = (g * tg) // 255
            b = (b * tb) // 255

    allowed = SOURCE_TILE_ALLOWED_PALETTE.get(tile)
    if not allowed:
        return PAL_TRANSPARENT

    best_idx = allowed[0]
    best_score = sys.maxsize
    for idx in allowed:
        pr, pg, pb = PREVIEW_PALETTE[idx]
        dr = r - pr
        dg = g - pg
        db = b - pb
        score = dr * dr + dg * dg + db * db
        if score < best_score:
            best_score = score
            best_idx = idx
    return best_idx


def _load_source_tile(tile: int) -> list[tuple[int, int, int, int]] | None:
    if tile in _SOURCE_TILE_CACHE:
        return _SOURCE_TILE_CACHE[tile]

    name = SOURCE_TEXTURE_FILES.get(tile)
    if name is None:
        return None

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return None

    root = Path(__file__).resolve().parents[1]
    source_path = root / "assets" / "minecraft_source" / name
    if not source_path.exists():
        return None

    with Image.open(source_path) as image:
        rgba = image.convert("RGBA")
        if rgba.size != (TILE_SIZE, TILE_SIZE):
            raise ValueError(f"{source_path} must be {TILE_SIZE}x{TILE_SIZE}, got {rgba.size}")
        pixels = list(rgba.getdata())
        _SOURCE_TILE_CACHE[tile] = pixels
        return pixels


def source_texel(tile: int, x: int, y: int) -> int | None:
    pixels = _load_source_tile(tile)
    if pixels is None:
        return None
    rgba = pixels[y * TILE_SIZE + x]
    return _quantize_to_palette(tile, rgba, x, y)


def grass_top(x: int, y: int) -> int:
    return pattern_texel(
        GRASS_TOP_ROWS,
        {"g": PAL_GRASS_TOP, "d": PAL_GRASS_DARK, "l": PAL_GRASS_LIGHT},
        x,
        y,
    )


def dirt(x: int, y: int) -> int:
    return pattern_texel(
        DIRT_ROWS,
        {"b": PAL_DIRT, "d": PAL_DIRT_DARK, "l": PAL_DIRT_LIGHT},
        x,
        y,
    )


def grass_side(x: int, y: int) -> int:
    return pattern_texel(
        GRASS_SIDE_ROWS,
        {
            "g": PAL_GRASS_SIDE,
            "d": PAL_GRASS_DARK,
            "l": PAL_GRASS_LIGHT,
            "b": PAL_DIRT,
        },
        x,
        y,
    )


def stone(x: int, y: int) -> int:
    return pattern_texel(
        STONE_ROWS,
        {"s": PAL_STONE, "d": PAL_STONE_DARK, "l": PAL_STONE_LIGHT},
        x,
        y,
    )


def wood_side(x: int, y: int) -> int:
    return pattern_texel(
        WOOD_SIDE_ROWS,
        {"b": PAL_WOOD, "g": PAL_WOOD_GRAIN, "d": PAL_WOOD_DARK},
        x,
        y,
    )


def glass(x: int, y: int) -> int:
    # Darker frame on the outside of the pane so adjacent glass blocks still
    # have a visible seam. Single-pixel highlight in the upper-left gives the
    # face a recognizable orientation so we can see that perspective-correct
    # UV mapping is still working through the alpha blend.
    if x == 0 or y == 0 or x == TILE_SIZE - 1 or y == TILE_SIZE - 1:
        return PAL_GLASS_EDGE
    if (x == 2 and 2 <= y <= 5) or (y == 2 and 2 <= x <= 5):
        return PAL_GLASS_HIGHLIGHT
    if (x == 3 and y == 3):
        return PAL_GLASS_HIGHLIGHT
    return PAL_GLASS


def lamp(x: int, y: int) -> int:
    if x == 0 or y == 0 or x == TILE_SIZE - 1 or y == TILE_SIZE - 1:
        return PAL_LAMP_FRAME
    if x in (2, TILE_SIZE - 3) or y in (2, TILE_SIZE - 3):
        return PAL_WOOD_DARK
    if 4 <= x <= 11 and 4 <= y <= 11:
        if 6 <= x <= 9 and 6 <= y <= 9:
            return PAL_WHITE
        return PAL_LAMP_GLOW
    if ((x + y) & 1) == 0:
        return PAL_WOOD_TOP
    return PAL_WOOD


def wood_top(x: int, y: int) -> int:
    # Square-ish rings read better at 16x16 than smooth circular math.
    dx = abs(x - 7.5)
    dy = abs(y - 7.5)
    r = int(max(dx, dy))
    skew = ((x * 3 + y * 5) & 3) == 0

    if r <= 1:
        return PAL_WOOD_DARK
    if (r + (1 if skew else 0)) % 4 == 0:
        return PAL_WOOD_GRAIN
    if (r + (1 if x > y else 0)) % 4 == 2:
        return PAL_WOOD
    return PAL_WOOD_TOP


def wood_plank(x: int, y: int) -> int:
    # Fallback if source PNG is missing.
    band = (y // 4) & 3
    if band == 0:
        return PAL_WOOD_TOP if (x & 1) == 0 else PAL_WOOD_GRAIN
    if band == 1:
        return PAL_WOOD if ((x + y) & 3) else PAL_WOOD_GRAIN
    if band == 2:
        return PAL_WOOD_GRAIN if (x & 1) else PAL_WOOD_TOP
    return PAL_WOOD if ((x + 2 * y) & 3) else PAL_WOOD_DARK


def leaves(x: int, y: int) -> int:
    # Includes sparse transparent texels for the dithered cutout look.
    return pattern_texel(
        LEAVES_ROWS,
        {
            "g": PAL_GRASS_SIDE,
            "d": PAL_GRASS_DARK,
            "l": PAL_GRASS_LIGHT,
            "b": PAL_DIRT,
            "t": PAL_TRANSPARENT,
        },
        x,
        y,
    )


def crosshair(x: int, y: int) -> int:
    if (x == 7 and 4 <= y <= 11) or (y == 7 and 4 <= x <= 11):
        return PAL_WHITE
    if (x == 8 and 5 <= y <= 10) or (y == 8 and 5 <= x <= 10):
        return PAL_WHITE
    return PAL_TRANSPARENT


def sky(x: int, y: int) -> int:
    if y <= 2:
        return PAL_TRANSPARENT
    if y <= 5:
        return PAL_SKY_HIGH
    if y <= 10:
        return PAL_SKY_MID
    return PAL_SKY_HORIZON


def cloud(x: int, y: int) -> int:
    covered = False

    for cx, cy, radius in ((3.0, 9.0, 3.4), (7.5, 7.0, 4.8), (12.0, 9.0, 3.6)):
        dx = x - cx
        dy = y - cy
        if dx * dx + dy * dy <= radius * radius:
            covered = True
            break

    if not covered:
        return PAL_TRANSPARENT
    if y >= 9 or (((x * 3) + y) & 3) == 0:
        return PAL_CLOUD_SHADOW
    return PAL_CLOUD


def sun(x: int, y: int) -> int:
    dx = x - 7.5
    dy = y - 7.5
    dist2 = dx * dx + dy * dy

    if dist2 <= 15.0:
        return PAL_SUN_CORE
    if dist2 <= 32.0:
        return PAL_SUN_GLOW
    return PAL_TRANSPARENT


def moon(x: int, y: int) -> int:
    dx = x - 7.0
    dy = y - 7.0
    dist2 = dx * dx + dy * dy

    if dist2 > 20.0:
        return PAL_TRANSPARENT

    cut_dx = x - 9.5
    cut_dy = y - 6.0
    if cut_dx * cut_dx + cut_dy * cut_dy <= 16.0:
        return PAL_TRANSPARENT
    if x <= 6 or y <= 5:
        return PAL_MOON_SHADOW
    return PAL_MOON


def stars(x: int, y: int) -> int:
    # Dense-but-structured starfield: a few twinkly "cross" stars plus
    # deterministic micro-clusters so the sky reads like stars, not noise.
    twinkles = {(3, 3), (12, 4), (5, 10), (11, 12)}

    if (x, y) in twinkles:
        return PAL_STAR
    for sx, sy in twinkles:
        if abs(x - sx) + abs(y - sy) == 1:
            return PAL_STAR if ((x + y + sx + sy) & 1) == 0 else PAL_TRANSPARENT

    n0 = noise(x, y, 73)
    n1 = noise(x + 5, y + 11, 157)

    if n0 >= 250:
        return PAL_STAR
    if n1 >= 247 and ((x + y) & 1) == 0:
        return PAL_STAR
    if n0 >= 242 and n1 >= 238 and ((x ^ y) & 1) == 0:
        return PAL_STAR
    return PAL_TRANSPARENT


def base_texel(tile: int, x: int, y: int) -> int:
    source_value = source_texel(tile, x, y)
    if source_value is not None:
        return source_value

    if tile == TEX_TILE_GRASS_TOP:
        return grass_top(x, y)
    if tile == TEX_TILE_GRASS_SIDE:
        return grass_side(x, y)
    if tile == TEX_TILE_DIRT:
        return dirt(x, y)
    if tile == TEX_TILE_STONE:
        return stone(x, y)
    if tile == TEX_TILE_WOOD_SIDE:
        return wood_side(x, y)
    if tile == TEX_TILE_WOOD_TOP:
        return wood_top(x, y)
    if tile == TEX_TILE_WOOD_PLANK:
        return wood_plank(x, y)
    if tile == TEX_TILE_GLASS:
        return glass(x, y)
    if tile == TEX_TILE_LAMP:
        return lamp(x, y)
    if tile == TEX_TILE_LEAVES:
        return leaves(x, y)
    if tile == TEX_TILE_SKY:
        return sky(x, y)
    if tile == TEX_TILE_CLOUD:
        return cloud(x, y)
    if tile == TEX_TILE_SUN:
        return sun(x, y)
    if tile == TEX_TILE_MOON:
        return moon(x, y)
    if tile == TEX_TILE_STARS:
        return stars(x, y)
    if tile == TEX_TILE_CROSSHAIR:
        return crosshair(x, y)
    return PAL_TRANSPARENT


def majority(samples: list[int]) -> int:
    best = samples[0]
    best_count = 1
    for value in samples:
        count = 0
        for sample in samples:
            if sample == value:
                count += 1
        if count > best_count:
            best = value
            best_count = count
    return best


def mip_texel(source_tile: int, x: int, y: int, block_shift: int) -> int:
    block_size = 1 << block_shift
    src_x = (x >> block_shift) << block_shift
    src_y = (y >> block_shift) << block_shift
    samples: list[int] = []

    for oy in range(block_size):
        for ox in range(block_size):
            samples.append(base_texel(source_tile, src_x + ox, src_y + oy))

    return majority(samples)


def texel(tile: int, x: int, y: int) -> int:
    if tile in MIP1_TILES:
        return mip_texel(MIP1_TILES[tile], x, y, 1)
    if tile in MIP2_TILES:
        return mip_texel(MIP2_TILES[tile], x, y, 2)
    return base_texel(tile, x, y)


def build_atlas() -> bytearray:
    atlas = bytearray(ATLAS_BYTES)

    for tile in range(TILE_COUNT):
        base = tile * TEXELS_PER_TILE
        for y in range(TILE_SIZE):
            for x in range(TILE_SIZE):
                atlas[base + y * TILE_SIZE + x] = texel(tile, x, y)

    return atlas


def write_mif(path: Path) -> None:
    """Altera Memory Initialization File for the texture ROM.

    `.mif` is the single source of truth for the texture atlas. Quartus
    consumes it via altsyncram `init_file` during synthesis, and the
    Python virtual hardware parses the same file at runtime (see
    `virtualhw.raster.load_texture_mif`).

    We picked `.mif` over Intel-HEX because the grammar is trivial
    (WIDTH/DEPTH/CONTENT BEGIN ... END;) so the Python parser stays in
    a handful of lines and has no checksum handling to get wrong.

    Layout matches ATLAS_BYTES = TILE_COUNT * TILE_SIZE * TILE_SIZE with
    WIDTH=8 and the same linear address ordering used by voxel_gpu.sv
    (tile << 8 | (v << 4) | u).
    """
    atlas = build_atlas()
    width = 8
    depth = len(atlas)
    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write("-- Auto-generated by generate_textures.py. Do not edit by hand.\n")
        handle.write(f"WIDTH = {width};\n")
        handle.write(f"DEPTH = {depth};\n\n")
        handle.write("ADDRESS_RADIX = HEX;\n")
        handle.write("DATA_RADIX    = HEX;\n\n")
        handle.write("CONTENT BEGIN\n")
        for addr, value in enumerate(atlas):
            handle.write(f"    {addr:04x} : {value:02x};\n")
        handle.write("END;\n")


def write_preview(path: Path, scale: int = 12, gap: int = 8, atlas_cols: int = 8) -> bool:
    """Render a PNG snapshot of the full texture atlas.

    The preview is a debug aid for humans, not a build input, so Pillow
    is a soft dependency: if PIL is not importable we just skip the PNG
    and let the .mif generation stand on its own. Returns True when a
    file was written, False otherwise.

    Layout:
      * all TILE_COUNT tiles are shown in row-major atlas order,
      * `atlas_cols` controls grid width (defaults to 8 columns),
      * each 16x16 tile is upscaled by `scale` with nearest-neighbour,
      * `gap` pixels of background colour separate the cells.

    Palette entries missing from PREVIEW_PALETTE render as bright
    magenta so drift between this table and the real renderer palette
    is impossible to miss.
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print(
            "skip preview: Pillow not installed (pip install Pillow) -- "
            "the .mif was still written"
        )
        return False

    atlas = build_atlas()
    cols = max(1, atlas_cols)
    rows = (TILE_COUNT + cols - 1) // cols
    cell = TILE_SIZE * scale
    width = gap + cols * (cell + gap)
    height = gap + rows * (cell + gap)
    bg = PREVIEW_PALETTE.get(PAL_TRANSPARENT, (0x10, 0x10, 0x18))
    missing = (0xff, 0x00, 0xff)

    image = Image.new("RGB", (width, height), bg)
    draw = ImageDraw.Draw(image)
    pixels = image.load()

    def paint(tile: int, cell_x: int, cell_y: int) -> None:
        base_addr = tile * TILE_SIZE * TILE_SIZE
        for ty in range(TILE_SIZE):
            for tx in range(TILE_SIZE):
                idx = atlas[base_addr + ty * TILE_SIZE + tx]
                rgb = PREVIEW_PALETTE.get(idx, missing)
                px0 = cell_x + tx * scale
                py0 = cell_y + ty * scale
                for oy in range(scale):
                    for ox in range(scale):
                        pixels[px0 + ox, py0 + oy] = rgb

    for tile in range(TILE_COUNT):
        row_index = tile // cols
        col_index = tile % cols
        cell_x = gap + col_index * (cell + gap)
        cell_y = gap + row_index * (cell + gap)
        paint(tile, cell_x, cell_y)

        # Label every atlas cell with tile index for debugging.
        # This keeps "is grass top wired?" questions answerable from the PNG.
        label = f"{tile:02d}"
        tx = cell_x + 2
        ty = cell_y + 2
        draw.rectangle((tx - 1, ty - 1, tx + 16, ty + 9), fill=(0x00, 0x00, 0x00))
        draw.text((tx, ty), label, fill=(0xff, 0xff, 0xff))

    image.save(path, format="PNG", optimize=True)
    return True


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    assets = root / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    mif_path = assets / "textures.mif"
    preview_path = assets / "textures_preview.png"

    write_mif(mif_path)
    print(f"wrote {mif_path}")

    if write_preview(preview_path):
        print(f"wrote {preview_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
