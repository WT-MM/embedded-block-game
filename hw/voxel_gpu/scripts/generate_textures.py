from __future__ import annotations

from pathlib import Path
import re
import sys

# Pillow is imported lazily inside write_preview() so the .mif emission
# path (which has no extra dependencies) still works on hosts that don't
# have PIL installed.

TILE_SIZE = 16
TILE_COUNT = 128
TEXELS_PER_TILE = TILE_SIZE * TILE_SIZE
ATLAS_BYTES = TILE_COUNT * TEXELS_PER_TILE

REPO_ROOT = Path(__file__).resolve().parents[3]
TEXTURE_TILE_DEF = REPO_ROOT / "sw" / "texture_tiles.def"


class TextureTile:
    def __init__(self, name: str, base: int, mip1: int | None = None, mip2: int | None = None):
        self.name = name
        self.base = base
        self.mip1 = mip1
        self.mip2 = mip2

    @property
    def label(self) -> str:
        return self.name.lower()


def load_texture_tiles(path: Path) -> list[TextureTile]:
    tile_re = re.compile(r"^\s*TEXTURE_TILE\(\s*([A-Z0-9_]+)\s*,\s*(\d+)\s*\)")
    mip_re = re.compile(
        r"^\s*TEXTURE_TILE_MIPPED\(\s*([A-Z0-9_]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
    )
    tiles: list[TextureTile] = []
    used: dict[int, str] = {}

    with path.open("r", encoding="ascii") as handle:
        for line_no, line in enumerate(handle, start=1):
            line = line.split("/*", 1)[0].strip()
            if not line or line.startswith("//"):
                continue

            match = mip_re.match(line)
            if match:
                name = match.group(1)
                ids = [int(match.group(i)) for i in range(2, 5)]
                for tile_id in ids:
                    if tile_id >= TILE_COUNT:
                        raise ValueError(f"{path}:{line_no}: tile {tile_id} exceeds atlas size {TILE_COUNT}")
                    if tile_id in used:
                        raise ValueError(f"{path}:{line_no}: tile {tile_id} reused by {name} and {used[tile_id]}")
                    used[tile_id] = name
                tiles.append(TextureTile(name, ids[0], ids[1], ids[2]))
                continue

            match = tile_re.match(line)
            if match:
                name = match.group(1)
                tile_id = int(match.group(2))
                if tile_id >= TILE_COUNT:
                    raise ValueError(f"{path}:{line_no}: tile {tile_id} exceeds atlas size {TILE_COUNT}")
                if tile_id in used:
                    raise ValueError(f"{path}:{line_no}: tile {tile_id} reused by {name} and {used[tile_id]}")
                used[tile_id] = name
                tiles.append(TextureTile(name, tile_id))
                continue

            raise ValueError(f"{path}:{line_no}: unrecognized texture metadata line: {line!r}")

    return tiles


TEXTURE_TILES = load_texture_tiles(TEXTURE_TILE_DEF)
MIPPED_TEXTURE_TILES = [tile for tile in TEXTURE_TILES if tile.mip1 is not None and tile.mip2 is not None]

for tile in TEXTURE_TILES:
    globals()[f"TEX_TILE_{tile.name}"] = tile.base
    if tile.mip1 is not None and tile.mip2 is not None:
        globals()[f"TEX_TILE_{tile.name}_MIP1"] = tile.mip1
        globals()[f"TEX_TILE_{tile.name}_MIP2"] = tile.mip2

BREAK_STAGE_TILES = (
    TEX_TILE_BREAK_0,
    TEX_TILE_BREAK_1,
    TEX_TILE_BREAK_2,
    TEX_TILE_BREAK_3,
    TEX_TILE_BREAK_4,
    TEX_TILE_BREAK_5,
    TEX_TILE_BREAK_6,
    TEX_TILE_BREAK_7,
    TEX_TILE_BREAK_8,
    TEX_TILE_BREAK_9,
)

PAL_TRANSPARENT = 0
PAL_GRASS_TOP = 1
PAL_DIRT = 2
PAL_WOOD = 3
PAL_STONE = 4
PAL_WHITE = 5
PAL_RED = 6
PAL_BLUE = 7
PAL_YELLOW = 8
PAL_GRASS_SIDE = 9
PAL_WOOD_TOP = 11
PAL_UI_DARK = 14
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
PAL_WATER_DEEP = 40
PAL_WATER_MID = 41
PAL_WATER_HIGHLIGHT = 42
PAL_LAVA_DARK = 43
PAL_LAVA_ORANGE = 44
PAL_LAVA_HOT = 45
PAL_SAND = 46
PAL_SAND_DARK = 47
PAL_BRICK = 48
PAL_BRICK_DARK = 49
PAL_OBSIDIAN = 50
PAL_OBSIDIAN_EDGE = 51
PAL_CLAY = 52
PAL_SAND_LIGHT = 53
PAL_SAND_SHADOW = 54
PAL_BRICK_LIGHT = 55
PAL_BRICK_MORTAR = 56


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
    40: (0x2a, 0x52, 0x9c),  # banked water deep
    41: (0x3a, 0x6c, 0xc4),  # banked water mid
    42: (0x6f, 0x9d, 0xe4),  # banked water highlight
    43: (0x7a, 0x20, 0x10),  # lava dark
    44: (0xe8, 0x5c, 0x18),  # lava orange
    45: (0xff, 0xd8, 0x5a),  # lava hot
    46: (0xd8, 0xc0, 0x74),  # sand
    47: (0xa8, 0x90, 0x50),  # sand dark
    48: (0xb8, 0x4d, 0x3f),  # brick
    49: (0x6e, 0x2a, 0x27),  # brick dark
    50: (0x20, 0x1b, 0x2c),  # obsidian
    51: (0x43, 0x36, 0x58),  # obsidian edge
    52: (0x72, 0x82, 0x91),  # clay
    53: (0xed, 0xdc, 0x9c),  # sand light
    54: (0xc3, 0xac, 0x67),  # sand shadow
    55: (0xd0, 0x68, 0x55),  # brick light
    56: (0x8b, 0x56, 0x4e),  # brick mortar
}

# Tiles shown in the preview, one column per block type, one row per LOD.
# Columns are chosen so the preview matches what a player actually sees on
# a real block face; add/remove entries as the atlas grows.
PREVIEW_COLUMNS: list[tuple[str, int, int, int]] = [
    (tile.label, tile.base, tile.mip1, tile.mip2)
    for tile in MIPPED_TEXTURE_TILES
]



MIP1_TILES = {
    tile.mip1: tile.base
    for tile in MIPPED_TEXTURE_TILES
}

MIP2_TILES = {
    tile.mip2: tile.base
    for tile in MIPPED_TEXTURE_TILES
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
    TEX_TILE_WATER: "water_still.png",
    TEX_TILE_CACTUS_SIDE: "cactus_side.tga",
    TEX_TILE_CACTUS_TOP: "cactus_top.tga",
    TEX_TILE_CACTUS_BOTTOM: "cactus_bottom.tga",
    TEX_TILE_DOOR_LOWER: "door_wood_lower.png",
    TEX_TILE_DOOR_UPPER: "door_wood_upper.png",
    TEX_TILE_COAL: "coal.png",
    TEX_TILE_TORCH: "torch_on.png",
    TEX_TILE_SAND: "sand.png",
    TEX_TILE_GRAVEL: "gravel.png",
    TEX_TILE_COBBLESTONE: "cobblestone.png",
    TEX_TILE_BRICKS: "brick.png",
    TEX_TILE_OBSIDIAN: "obsidian.png",
    TEX_TILE_SANDSTONE: "sandstone_normal.png",
    TEX_TILE_CLAY: "clay.png",
    TEX_TILE_REDSTONE_BLOCK: "redstone_block.png",
    TEX_TILE_LAVA: "lava_still.png",
    TEX_TILE_COAL_ORE: "coal_ore.png",
    TEX_TILE_IRON_ORE: "iron_ore.png",
    TEX_TILE_GOLD_ORE: "gold_ore.png",
    TEX_TILE_DIAMOND_ORE: "diamond_ore.png",
    TEX_TILE_REDSTONE_ORE: "redstone_ore.png",
    TEX_TILE_GOLD_BLOCK: "gold_block.png",
    TEX_TILE_DIAMOND_BLOCK: "diamond_block.png",
    TEX_TILE_RED_FLOWER: "flower_rose.png",
    TEX_TILE_YELLOW_FLOWER: "flower_dandelion.png",
    TEX_TILE_RED_MUSHROOM: "mushroom_red.png",
    TEX_TILE_BROWN_MUSHROOM: "mushroom_brown.png",
    TEX_TILE_APPLE: "apple.png",
    TEX_TILE_BOWL: "bowl.png",
    TEX_TILE_MUSHROOM_STEW: "mushroom_stew.png",
    TEX_TILE_STICK: "stick.png",
    TEX_TILE_DOOR_ITEM: "door_wood.png",
    TEX_TILE_CRAFTING_TABLE_TOP: "crafting_table_top.png",
    TEX_TILE_CRAFTING_TABLE_SIDE: "crafting_table_side.png",
    TEX_TILE_CRAFTING_TABLE_FRONT: "crafting_table_front.png",
    TEX_TILE_FURNACE_TOP: "furnace_top.png",
    TEX_TILE_FURNACE_SIDE: "furnace_side.png",
    TEX_TILE_FURNACE_FRONT: "furnace_front_off.png",
    TEX_TILE_HEART_CONTAINER: "container.png",
    TEX_TILE_HEART: "full.png",
    TEX_TILE_DRUMSTICK: "food_full.png",
    TEX_TILE_DRUMSTICK_EMPTY: "food_full.png",
    TEX_TILE_AIR_BUBBLE: "air.png",
    TEX_TILE_AIR_BUBBLE_POP: "air_bursting.png",
    TEX_TILE_HEART_HALF: "heart_half.png",
    TEX_TILE_HEART_BLINK: "heart_full_blinking.png",
    TEX_TILE_HEART_HALF_BLINK: "heart_half_blinking.png",
}

CACTUS_SOURCE_TILES = {
    TEX_TILE_CACTUS_SIDE,
    TEX_TILE_CACTUS_TOP,
    TEX_TILE_CACTUS_BOTTOM,
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
    # Restrict leaves to two greens for opaque texels. We deliberately exclude
    # PAL_TRANSPARENT (0) here even though leaves use cutout transparency:
    # genuinely-transparent texels short-circuit at the alpha<96 check at the
    # top of _quantize_to_palette, so they never reach this list. Including 0
    # would let nearest-color route any sufficiently dark opaque pixel to the
    # background color (because the dark forest tint is closer to (16,16,24)
    # than to (92,134,52)) and the leaves would render as solid sky blobs.
    TEX_TILE_LEAVES: (9, 17),
    TEX_TILE_CACTUS_SIDE: (PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_GRASS_LIGHT, PAL_WHITE),
    TEX_TILE_CACTUS_TOP: (PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_GRASS_LIGHT, PAL_WHITE),
    TEX_TILE_CACTUS_BOTTOM: (PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_GRASS_LIGHT, PAL_WHITE),
    TEX_TILE_DOOR_LOWER: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_DOOR_UPPER: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_COAL: (PAL_TRANSPARENT, PAL_OBSIDIAN, PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE),
    TEX_TILE_TORCH: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_LAVA_DARK, PAL_LAVA_ORANGE, PAL_LAVA_HOT, PAL_SUN_GLOW, PAL_SUN_CORE, PAL_WHITE),
    # Water: the vanilla texture is a grayscale brightness mask (alpha=180).
    # Map bright pixels to highlight and dark pixels to deep blue. The mid
    # entry gives the body tone. Semi-transparent pixels (alpha<96) won't reach
    # this list because the quantizer early-exits to PAL_TRANSPARENT first.
    TEX_TILE_WATER: (PAL_WATER_DEEP, PAL_WATER_MID, PAL_WATER_HIGHLIGHT),
    TEX_TILE_SAND: (PAL_SAND_DARK, PAL_SAND_SHADOW, PAL_SAND, PAL_SAND_LIGHT),
    TEX_TILE_GRAVEL: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_COBBLESTONE: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_BRICKS: (PAL_BRICK_DARK, PAL_BRICK_MORTAR, PAL_BRICK, PAL_BRICK_LIGHT),
    TEX_TILE_OBSIDIAN: (PAL_UI_DARK, PAL_OBSIDIAN, PAL_OBSIDIAN_EDGE),
    TEX_TILE_SANDSTONE: (PAL_SAND_DARK, PAL_SAND_SHADOW, PAL_SAND, PAL_SAND_LIGHT),
    TEX_TILE_CLAY: (PAL_GLASS_EDGE, PAL_CLAY, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_REDSTONE_BLOCK: (PAL_BRICK_DARK, PAL_BRICK, PAL_RED, PAL_LAVA_ORANGE),
    TEX_TILE_LAVA: (PAL_LAVA_DARK, PAL_LAVA_ORANGE, PAL_LAVA_HOT, PAL_SUN_GLOW, PAL_SUN_CORE),
    TEX_TILE_COAL_ORE: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_IRON_ORE: (PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT, PAL_DIRT_LIGHT, PAL_SAND),
    TEX_TILE_GOLD_ORE: (PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT, PAL_YELLOW, PAL_SUN_CORE),
    TEX_TILE_DIAMOND_ORE: (PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT, PAL_BLUE, PAL_GLASS_HIGHLIGHT, PAL_WHITE),
    TEX_TILE_REDSTONE_ORE: (PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT, PAL_RED, PAL_LAVA_ORANGE),
    TEX_TILE_GOLD_BLOCK: (PAL_LAMP_GLOW, PAL_YELLOW, PAL_SUN_CORE, PAL_WHITE),
    TEX_TILE_DIAMOND_BLOCK: (PAL_BLUE, PAL_GLASS, PAL_GLASS_HIGHLIGHT, PAL_WHITE),
    TEX_TILE_RED_FLOWER: (PAL_TRANSPARENT, PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_RED, PAL_WHITE),
    TEX_TILE_YELLOW_FLOWER: (PAL_TRANSPARENT, PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_YELLOW, PAL_SUN_CORE),
    TEX_TILE_RED_MUSHROOM: (PAL_TRANSPARENT, PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_RED, PAL_WHITE, PAL_WOOD_TOP),
    TEX_TILE_BROWN_MUSHROOM: (PAL_TRANSPARENT, PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_APPLE: (PAL_TRANSPARENT, PAL_GRASS_DARK, PAL_GRASS_SIDE, PAL_RED, PAL_WHITE),
    TEX_TILE_BOWL: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_MUSHROOM_STEW: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT, PAL_RED, PAL_WHITE),
    TEX_TILE_STICK: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_DOOR_ITEM: (PAL_TRANSPARENT, PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT),
    TEX_TILE_CRAFTING_TABLE_TOP: (PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT, PAL_STONE_DARK, PAL_STONE_LIGHT, PAL_RED, PAL_WHITE),
    TEX_TILE_CRAFTING_TABLE_SIDE: (PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT, PAL_STONE_DARK, PAL_STONE_LIGHT, PAL_RED, PAL_WHITE),
    TEX_TILE_CRAFTING_TABLE_FRONT: (PAL_WOOD_DARK, PAL_WOOD, PAL_WOOD_TOP, PAL_DIRT_LIGHT, PAL_STONE_DARK, PAL_STONE_LIGHT, PAL_RED, PAL_WHITE),
    TEX_TILE_FURNACE_TOP: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_FURNACE_SIDE: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT),
    TEX_TILE_FURNACE_FRONT: (PAL_UI_DARK, PAL_STONE_DARK, PAL_STONE, PAL_STONE_LIGHT, PAL_LAVA_DARK, PAL_LAVA_ORANGE),
    TEX_TILE_HEART_CONTAINER: (14,),
    TEX_TILE_HEART: (14, 6, 5),
    TEX_TILE_HEART_HALF: (14, 6, 5),
    TEX_TILE_HEART_BLINK: (14, 6, 5),
    TEX_TILE_HEART_HALF_BLINK: (14, 6, 5),
    TEX_TILE_DRUMSTICK_EMPTY: (PAL_OBSIDIAN,),
    TEX_TILE_DRUMSTICK: (14, 2, 11, 15, 16, 24, 5),
    TEX_TILE_AIR_BUBBLE: (14, PAL_WATER_DEEP, PAL_WATER_MID, PAL_WATER_HIGHLIGHT, 5),
    TEX_TILE_AIR_BUBBLE_POP: (14, PAL_WATER_DEEP, PAL_WATER_MID, PAL_WATER_HIGHLIGHT, 5),
}

HUD_SOURCE_TILES = {
    TEX_TILE_HEART,
    TEX_TILE_HEART_CONTAINER,
    TEX_TILE_DRUMSTICK,
    TEX_TILE_DRUMSTICK_EMPTY,
    TEX_TILE_AIR_BUBBLE,
    TEX_TILE_AIR_BUBBLE_POP,
    TEX_TILE_HEART_HALF,
    TEX_TILE_HEART_BLINK,
    TEX_TILE_HEART_HALF_BLINK,
}

HUD_CROP_SOURCE_TILES = {
    TEX_TILE_DRUMSTICK,
    TEX_TILE_DRUMSTICK_EMPTY,
}

# Vanilla grass/leaves/water textures are grayscale masks intended for biome tinting.
# Apply fixed tints in this project since we do not have biome maps.
# Leaves use a darker forest-green than grass so trees read as a distinct,
# slightly shadowed canopy against the brighter ground cover.
# Water uses a rich ocean-blue tint so the grayscale brightness mask maps
# to our three blue palette entries naturally via nearest-color quantization.
SOURCE_TILE_TINT: dict[int, tuple[int, int, int]] = {
    TEX_TILE_GRASS_TOP: (121, 192, 90),
    TEX_TILE_GRASS_SIDE: (121, 192, 90),
    TEX_TILE_LEAVES: (52, 96, 38),
    TEX_TILE_WATER: (42, 82, 156),   # tint maps gray→blue; bright→highlight, dark→deep
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

    if tile == TEX_TILE_WATER:
        # Vanilla water_still.png is a grayscale brightness mask (alpha ~180,
        # R/G/B all identical in range 165-255). Map brightness directly to
        # the three blue palette entries so the ripple highlights from the
        # source texture come through faithfully.
        gray = (r + g + b) // 3
        if gray >= 220:
            return PAL_WATER_HIGHLIGHT  # brightest ripple crests
        if gray >= 190:
            return PAL_WATER_MID        # mid-level water body
        return PAL_WATER_DEEP           # darkest troughs

    if tile == TEX_TILE_AIR_BUBBLE or tile == TEX_TILE_AIR_BUBBLE_POP:
        luma = (r * 30 + g * 59 + b * 11) // 100
        if luma < 20:
            return PAL_UI_DARK
        if r >= 180:
            return PAL_WHITE
        if r >= 40:
            return PAL_WATER_HIGHLIGHT
        return PAL_WATER_MID

    if tile == TEX_TILE_DRUMSTICK_EMPTY:
        return PAL_OBSIDIAN

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
        if tile in HUD_CROP_SOURCE_TILES:
            bbox = rgba.getbbox()
            if bbox is not None:
                nearest = getattr(Image, "Resampling", Image).NEAREST
                rgba = rgba.crop(bbox).resize((TILE_SIZE, TILE_SIZE), nearest)
        elif tile in HUD_SOURCE_TILES:
            # Modern vanilla HUD sprites are usually 9x9 standalone images.
            # Resize the whole sprite frame, not the non-transparent bounds:
            # half-heart and bursting-bubble sprites rely on empty pixels for
            # their actual shape and position.
            if rgba.size != (TILE_SIZE, TILE_SIZE):
                nearest = getattr(Image, "Resampling", Image).NEAREST
                rgba = rgba.resize((TILE_SIZE, TILE_SIZE), nearest)
        elif rgba.size[0] == TILE_SIZE and rgba.size[1] > TILE_SIZE and rgba.size[1] % TILE_SIZE == 0:
            # Animated vanilla block textures, such as lava_still.png, are
            # vertical frame strips. Our hardware atlas stores one static
            # tile, so use the first frame.
            rgba = rgba.crop((0, 0, TILE_SIZE, TILE_SIZE))
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
    if tile in CACTUS_SOURCE_TILES and rgba[3] < 96:
        best_rgba = rgba
        best_dist = TILE_SIZE * TILE_SIZE * 2
        for sy in range(TILE_SIZE):
            for sx in range(TILE_SIZE):
                candidate = pixels[sy * TILE_SIZE + sx]
                if candidate[3] < 96:
                    continue
                dx = sx - x
                dy = sy - y
                dist = dx * dx + dy * dy
                if dist < best_dist:
                    best_rgba = candidate
                    best_dist = dist
        r, g, b, _ = best_rgba
        rgba = (r, g, b, 255)
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


_WATER_RIPPLE_OFFSETS = (0, 1, 1, 2, 2, 1, 1, 0, -1, -2, -2, -3, -3, -2, -2, -1)
_WATER_RIPPLE_LEN = len(_WATER_RIPPLE_OFFSETS)


def water(x: int, y: int) -> int:
    # Mostly-flat blue body with two superimposed sine-like horizontal ripple
    # lines that tile cleanly across 16x16 borders, plus sparse highlight
    # texels for sparkle. Translucent rendering relies on QUAD_ALPHA_50, so
    # the body texels stay fully opaque palette entries. The ripples thread
    # through the texture rather than sit as parallel diagonals so the
    # surface reads as water instead of a striped flag.
    if (x ^ (y * 5)) & 0x1F == 7:
        return PAL_WATER_HIGHLIGHT

    ripple_a = (y + _WATER_RIPPLE_OFFSETS[x % _WATER_RIPPLE_LEN]) & 0x0F
    if ripple_a == 4:
        return PAL_WATER_DEEP
    ripple_b = (y + _WATER_RIPPLE_OFFSETS[(x + 8) % _WATER_RIPPLE_LEN] + 8) & 0x0F
    if ripple_b == 4:
        return PAL_WATER_DEEP

    # A few mid-tone speckles to break up the flat fill.
    n = noise(x, y, 311)
    if n >= 224:
        return PAL_WATER_HIGHLIGHT
    if n >= 200:
        return PAL_WATER_DEEP
    return PAL_WATER_MID


def cactus(x: int, y: int, top: bool = False) -> int:
    if x == 0 or y == 0 or x == TILE_SIZE - 1 or y == TILE_SIZE - 1:
        return PAL_GRASS_DARK
    if top and 4 <= x <= 11 and 4 <= y <= 11:
        return PAL_GRASS_LIGHT if noise(x, y, 377) > 128 else PAL_GRASS_SIDE
    if x in (3, 12) and (y & 1) == 0:
        return PAL_WHITE
    if noise(x, y, 379) > 210:
        return PAL_GRASS_LIGHT
    return PAL_GRASS_SIDE


def sand(x: int, y: int) -> int:
    n = noise(x, y, 421)
    if n < 34 or ((x + y * 3) & 0x0F) == 0:
        return PAL_SAND_DARK
    if n > 218:
        return PAL_SAND_LIGHT
    if n < 72:
        return PAL_SAND_SHADOW
    return PAL_SAND


def gravel(x: int, y: int) -> int:
    n = noise(x, y, 433)
    if n < 72:
        return PAL_STONE_DARK
    if n > 196:
        return PAL_STONE_LIGHT
    if ((x * 5 + y * 3) & 7) == 0:
        return PAL_UI_DARK
    return PAL_STONE


def cobblestone(x: int, y: int) -> int:
    cell_x = x // 4
    cell_y = y // 4
    local_x = x & 3
    local_y = y & 3
    offset = (cell_y & 1) * 2
    seam = local_y == 0 or ((x + offset) & 3) == 0

    if seam:
        return PAL_STONE_DARK
    if noise(cell_x, cell_y, 467) > 170:
        return PAL_STONE_LIGHT
    return PAL_STONE


def bricks(x: int, y: int) -> int:
    row = y // 4
    shifted_x = (x + ((row & 1) * 4)) & 15
    mortar = (y & 3) == 0 or (shifted_x & 7) == 0

    if mortar:
        return PAL_BRICK_MORTAR
    if noise(x, y, 479) > 204:
        return PAL_BRICK_LIGHT
    if noise(y, x, 481) < 40:
        return PAL_BRICK_DARK
    return PAL_BRICK


def obsidian(x: int, y: int) -> int:
    n = noise(x, y, 491)
    if x == 0 or y == 0 or x == 15 or y == 15:
        return PAL_OBSIDIAN_EDGE
    if n > 232:
        return PAL_OBSIDIAN_EDGE
    if ((x + 2 * y) & 15) == 0:
        return PAL_UI_DARK
    return PAL_OBSIDIAN


def sandstone(x: int, y: int) -> int:
    if y in (0, 7, 15):
        return PAL_SAND_DARK
    if y == 8 and 3 <= x <= 12:
        return PAL_SAND_SHADOW
    if noise(x, y, 503) < 42:
        return PAL_SAND_SHADOW
    if noise(y, x, 509) > 224:
        return PAL_SAND_LIGHT
    return PAL_SAND


def clay(x: int, y: int) -> int:
    n = noise(x, y, 521)
    if n < 64:
        return PAL_GLASS_EDGE
    if n > 220:
        return PAL_STONE_LIGHT
    return PAL_CLAY


def redstone_block(x: int, y: int) -> int:
    if x == 0 or y == 0 or x == 15 or y == 15:
        return PAL_BRICK_DARK
    if x in (4, 11) or y in (4, 11):
        return PAL_RED
    if noise(x, y, 541) > 220:
        return PAL_LAVA_ORANGE
    return PAL_BRICK


def lava(x: int, y: int) -> int:
    n = noise(x, y, 557)
    vein_a = (x + y + (noise(y, x, 563) >> 6)) & 7
    vein_b = (x * 2 - y + 16 + (noise(x, y, 569) >> 7)) & 15

    if vein_a <= 1 or vein_b <= 1:
        return PAL_LAVA_HOT if n > 64 else PAL_SUN_GLOW
    if n > 190:
        return PAL_LAVA_HOT
    if n < 42:
        return PAL_LAVA_DARK
    return PAL_LAVA_ORANGE


def ore_texture(x: int, y: int, accent: int, highlight: int, seed: int) -> int:
    n = noise(x, y, seed)
    cluster = ((x - 5) * (x - 5) + (y - 5) * (y - 5) <= 9 or
               (x - 11) * (x - 11) + (y - 10) * (y - 10) <= 8 or
               (x - 4) * (x - 4) + (y - 12) * (y - 12) <= 5)

    if cluster and n > 72:
        return highlight if n > 196 else accent
    if n < 26:
        return PAL_STONE_DARK
    if n > 236:
        return PAL_STONE_LIGHT
    return PAL_STONE


def gold_block(x: int, y: int) -> int:
    if x == 0 or y == 0 or x == 15 or y == 15:
        return PAL_LAMP_GLOW
    if x in (5, 10) or y in (5, 10):
        return PAL_YELLOW
    if noise(x, y, 587) > 220:
        return PAL_SUN_CORE
    return PAL_LAMP_GLOW


def diamond_block(x: int, y: int) -> int:
    if x == 0 or y == 0 or x == 15 or y == 15:
        return PAL_BLUE
    if x in (4, 11) or y in (4, 11):
        return PAL_GLASS_HIGHLIGHT
    if noise(x, y, 599) > 218:
        return PAL_WHITE
    return PAL_GLASS


def flower(x: int, y: int, petal: int, petal_highlight: int) -> int:
    stem = x in (7, 8) and 7 <= y <= 15
    leaf = ((4 <= x <= 7 and y in (11, 12) and x + y <= 18) or
            (8 <= x <= 12 and y in (10, 11) and x - y <= 1))
    petal_shape = ((x - 7) * (x - 7) + (y - 4) * (y - 4) <= 8 or
                   (x - 5) * (x - 5) + (y - 5) * (y - 5) <= 4 or
                   (x - 10) * (x - 10) + (y - 5) * (y - 5) <= 4)

    if petal_shape:
        return petal_highlight if noise(x, y, 613) > 210 else petal
    if stem or leaf:
        return PAL_GRASS_DARK if noise(x, y, 617) < 92 else PAL_GRASS_SIDE
    return PAL_TRANSPARENT


def crosshair(x: int, y: int) -> int:
    if (x == 7 and 4 <= y <= 11) or (y == 7 and 4 <= x <= 11):
        return PAL_WHITE
    if (x == 8 and 5 <= y <= 10) or (y == 8 and 5 <= x <= 10):
        return PAL_WHITE
    return PAL_TRANSPARENT


AIR_BUBBLE_ROWS = [
    "tttttwwwwttttttt",
    "tttwwbbbbwwttttt",
    "ttwbwwbbbbbwtttt",
    "twbwwbbbbbbbwttt",
    "twbbbbbbbbbbbwtt",
    "wbbbbbbbbbbbbwtt",
    "wbbbbbbbbbbbbwtt",
    "wbbbbbbbbbbbbwtt",
    "wbbbbbbbbbbbbwtt",
    "twbbbbbbbbbbwttt",
    "twbbbbbbbbbwtttt",
    "ttwbbbbbbwtttttt",
    "tttwbbbbwttttttt",
    "ttttwwwwtttttttt",
    "tttttttttttttttt",
    "tttttttttttttttt",
]

AIR_BUBBLE_POP_ROWS = [
    "ttttttwwtttttttt",
    "tttwttttttwttttt",
    "ttttwttttwtttttt",
    "tttttttttttttttt",
    "ttwtttttttttwttt",
    "ttttttwwtttttttt",
    "tttttwbbwwtttttt",
    "tttttwbbbwtttttt",
    "ttttttwwwttttttt",
    "tttttttttttttttt",
    "tttwttttttwttttt",
    "ttttwttttwtttttt",
    "ttttttwwtttttttt",
    "tttttttttttttttt",
    "tttttttttttttttt",
    "tttttttttttttttt",
]


def air_bubble(x: int, y: int) -> int:
    return pattern_texel(
        AIR_BUBBLE_ROWS,
        {"t": PAL_TRANSPARENT, "b": PAL_WATER_MID, "w": PAL_WHITE},
        x,
        y,
    )


def air_bubble_pop(x: int, y: int) -> int:
    return pattern_texel(
        AIR_BUBBLE_POP_ROWS,
        {"t": PAL_TRANSPARENT, "b": PAL_WATER_MID, "w": PAL_WHITE},
        x,
        y,
    )


BREAK_CRACK_SEGMENTS: tuple[tuple[int, float, float, float, float], ...] = (
    (0, 7.5, 7.5, 7.5, 4.8),
    (1, 7.5, 5.4, 4.8, 4.0),
    (2, 7.3, 6.0, 10.5, 4.2),
    (3, 7.5, 7.2, 5.3, 10.4),
    (4, 8.0, 7.5, 11.6, 9.7),
    (5, 5.6, 10.0, 3.4, 13.2),
    (6, 10.8, 4.5, 13.5, 2.2),
    (6, 10.9, 9.4, 13.7, 12.2),
    (7, 4.9, 4.2, 2.2, 2.0),
    (7, 7.5, 4.8, 7.0, 1.7),
    (8, 13.2, 12.0, 15.0, 14.5),
    (8, 3.5, 13.0, 1.2, 15.0),
    (9, 2.5, 2.4, 0.4, 0.9),
    (9, 13.0, 2.6, 15.0, 0.8),
)


def distance_to_segment(px: float, py: float,
                        x0: float, y0: float,
                        x1: float, y1: float) -> float:
    dx = x1 - x0
    dy = y1 - y0
    length_sq = dx * dx + dy * dy
    if length_sq <= 0.0001:
        return ((px - x0) * (px - x0) + (py - y0) * (py - y0)) ** 0.5

    t = ((px - x0) * dx + (py - y0) * dy) / length_sq
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0

    cx = x0 + dx * t
    cy = y0 + dy * t
    return ((px - cx) * (px - cx) + (py - cy) * (py - cy)) ** 0.5


def break_stage(stage: int, x: int, y: int) -> int:
    px = float(x) + 0.5
    py = float(y) + 0.5
    width = 0.34 + 0.035 * float(stage)
    nearest = 99.0

    for unlock_stage, x0, y0, x1, y1 in BREAK_CRACK_SEGMENTS:
        if unlock_stage > stage:
            continue
        dist = distance_to_segment(px, py, x0, y0, x1, y1)
        if dist < nearest:
            nearest = dist

    if nearest <= width:
        return PAL_STONE_DARK
    if stage >= 5 and nearest <= width + 0.22 and noise(x, y, 991 + stage) > 170:
        return PAL_STONE_DARK
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
    if tile == TEX_TILE_WATER:
        return water(x, y)
    if tile == TEX_TILE_CACTUS_SIDE:
        return cactus(x, y, False)
    if tile == TEX_TILE_CACTUS_TOP or tile == TEX_TILE_CACTUS_BOTTOM:
        return cactus(x, y, True)
    if tile == TEX_TILE_SAND:
        return sand(x, y)
    if tile == TEX_TILE_GRAVEL:
        return gravel(x, y)
    if tile == TEX_TILE_COBBLESTONE:
        return cobblestone(x, y)
    if tile == TEX_TILE_BRICKS:
        return bricks(x, y)
    if tile == TEX_TILE_OBSIDIAN:
        return obsidian(x, y)
    if tile == TEX_TILE_SANDSTONE:
        return sandstone(x, y)
    if tile == TEX_TILE_CLAY:
        return clay(x, y)
    if tile == TEX_TILE_REDSTONE_BLOCK:
        return redstone_block(x, y)
    if tile == TEX_TILE_LAVA:
        return lava(x, y)
    if tile == TEX_TILE_COAL_ORE:
        return ore_texture(x, y, PAL_UI_DARK, PAL_STONE_DARK, 631)
    if tile == TEX_TILE_IRON_ORE:
        return ore_texture(x, y, PAL_DIRT_LIGHT, PAL_SAND, 641)
    if tile == TEX_TILE_GOLD_ORE:
        return ore_texture(x, y, PAL_YELLOW, PAL_SUN_CORE, 643)
    if tile == TEX_TILE_DIAMOND_ORE:
        return ore_texture(x, y, PAL_BLUE, PAL_GLASS_HIGHLIGHT, 647)
    if tile == TEX_TILE_REDSTONE_ORE:
        return ore_texture(x, y, PAL_RED, PAL_LAVA_ORANGE, 653)
    if tile == TEX_TILE_GOLD_BLOCK:
        return gold_block(x, y)
    if tile == TEX_TILE_DIAMOND_BLOCK:
        return diamond_block(x, y)
    if tile == TEX_TILE_RED_FLOWER:
        return flower(x, y, PAL_RED, PAL_WHITE)
    if tile == TEX_TILE_YELLOW_FLOWER:
        return flower(x, y, PAL_YELLOW, PAL_SUN_CORE)
    if tile == TEX_TILE_RED_MUSHROOM:
        return flower(x, y, PAL_RED, PAL_WHITE)
    if tile == TEX_TILE_BROWN_MUSHROOM:
        return flower(x, y, PAL_WOOD, PAL_DIRT_LIGHT)
    if tile == TEX_TILE_APPLE:
        return flower(x, y, PAL_RED, PAL_WHITE)
    if tile == TEX_TILE_BOWL:
        return wood_top(x, y)
    if tile == TEX_TILE_MUSHROOM_STEW:
        return wood_plank(x, y)
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
    if tile == TEX_TILE_AIR_BUBBLE:
        return air_bubble(x, y)
    if tile == TEX_TILE_AIR_BUBBLE_POP:
        return air_bubble_pop(x, y)
    if tile == TEX_TILE_CROSSHAIR:
        return crosshair(x, y)
    if tile in BREAK_STAGE_TILES:
        return break_stage(BREAK_STAGE_TILES.index(tile), x, y)
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
