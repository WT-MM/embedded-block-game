from __future__ import annotations

from pathlib import Path

# Pillow is imported lazily inside write_preview() so the .mif emission
# path (which has no extra dependencies) still works on hosts that don't
# have PIL installed.

TILE_SIZE = 16
TILE_COUNT = 64
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
    1:  (0x6b, 0xa4, 0x3a),  # grass top
    2:  (0x8b, 0x63, 0x41),  # dirt side
    3:  (0x6f, 0x57, 0x37),  # wood side
    4:  (0x7c, 0x7c, 0x7c),  # stone side
    5:  (0xff, 0xff, 0xff),  # debug white
    6:  (0xff, 0x40, 0x40),  # debug red
    7:  (0x40, 0xa0, 0xff),  # debug blue
    8:  (0xff, 0xd0, 0x40),  # debug yellow
    9:  (0x5c, 0x86, 0x34),  # grass side
    10: (0x6a, 0x4a, 0x2c),  # grass bottom / dark dirt
    11: (0x9d, 0x7b, 0x4d),  # wood top
    12: (0x53, 0x38, 0x23),  # wood bottom
    13: (0x98, 0x98, 0x98),  # stone top
    14: (0x5c, 0x5c, 0x5c),  # stone bottom
    15: (0xa7, 0x79, 0x52),  # dirt top
    16: (0x59, 0x41, 0x2a),  # dirt bottom
    17: (0x4f, 0x78, 0x2d),  # grass dark
    18: (0x84, 0xba, 0x57),  # grass highlight
    19: (0x6f, 0x4f, 0x32),  # dirt dark
    20: (0xaa, 0x81, 0x5a),  # dirt light
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
}

GRASS_TOP_ROWS = [
    "ggddggggllggggdg",
    "ggddggggllggddgg",
    "dgggggddggggllgg",
    "dgggllggggddgggg",
    "ggggllggggddgggg",
    "ggddggggggggggll",
    "ggddggllggggggll",
    "ggggggllggddgggg",
    "llggggggggddgggg",
    "llggddggggggggdd",
    "ggggddggllggggdd",
    "ggggggggllgggggg",
    "ddggggggggllgggg",
    "ggllggddgggggggg",
    "ggllggddggggllgg",
    "ggggggggddggllgg",
]

DIRT_ROWS = [
    "bbddbbbbllbbbbbb",
    "bbddbbbbllbbddbb",
    "ddbbbbbbbbbblldb",
    "ddbbbbllbbbbbbdb",
    "bbbbbbllbbbbddbb",
    "bbbddbbbbbbbddbb",
    "bbbddbbllbbbbbbb",
    "llbbbbbbbbbddbbb",
    "llbbbbddbbbbbbbb",
    "bbbbbbbbllbbbbdd",
    "bbddbbbbllddbbbb",
    "bbbbllbbbbddbbbb",
    "ddbbbbbbbbbbllbb",
    "ddbbllbbbbbbbbbb",
    "bbbbllbbbbddbbll",
    "bbbbbbbbddbbllbb",
]

GRASS_SIDE_ROWS = [
    "ggddggggllgggggg",
    "ggddggggllggddgg",
    "dgggggddggggllgg",
    "bbggllbbggddbbgg",
    "bbddbbbbllbbbbbb",
    "bbddbbbbllbbddbb",
    "ddbbbbbbbbbblldb",
    "ddbbbbllbbbbbbdb",
    "bbbbbbllbbbbddbb",
    "bbbddbbbbbbbddbb",
    "bbbddbbllbbbbbbb",
    "llbbbbbbbbbddbbb",
    "llbbbbddbbbbbbbb",
    "bbbbbbbbllbbbbdd",
    "bbddbbbbllddbbbb",
    "bbbbllbbbbddbbbb",
]

STONE_ROWS = [
    "ssddssssllssssss",
    "ssddssssllssddss",
    "ddsssssssssllssd",
    "ddssssllssssssds",
    "ssssssllssssddss",
    "sssddsssssssddss",
    "sssddssllsssssss",
    "llsssssssssddsss",
    "llssssddssssssss",
    "ssssssssllssssdd",
    "ssddssssllddssss",
    "ssssllssssddssss",
    "ddssssssssssllss",
    "ddssllssssssssss",
    "ssssllssssddssll",
    "ssssssssddssllss",
]

WOOD_SIDE_ROWS = [
    "dgbbbggbbbgbbbgd",
    "dgbbgbbbggbbbggd",
    "dbgbbbggbbbgbbgd",
    "dggbbbgbbbggbbgd",
    "dgbbbggbbbgbbbgd",
    "dgbbgbbbggbbbggd",
    "dbgbbbggddggbbgd",
    "dggbbbgbddddbbgd",
    "dgbbbggbddddbbgd",
    "dgbbgbbbddggbggd",
    "dbgbbbggbbbgbbgd",
    "dggbbbgbbbggbbgd",
    "dgbbbggbbbgbbbgd",
    "dgbbgbbbggbbbggd",
    "dbgbbbggbbbgbbgd",
    "dggbbbgbbbggbbgd",
]


def pattern_texel(rows: list[str], mapping: dict[str, int], x: int, y: int) -> int:
    return mapping[rows[y][x]]


def noise(x: int, y: int, seed: int) -> int:
    value = (x * 97) ^ (y * 57) ^ (seed * 131)
    value = (value * 1103515245 + 12345) & 0x7FFFFFFF
    return value & 0xFF


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
    points = {
        (2, 3), (5, 11), (8, 6), (12, 2), (13, 9),
        (4, 7), (10, 13), (14, 5),
    }

    if (x, y) in points:
        return PAL_STAR
    if (x - 1, y) in points or (x, y - 1) in points:
        return PAL_STAR if ((x + y) & 1) == 0 else PAL_TRANSPARENT
    return PAL_TRANSPARENT


def base_texel(tile: int, x: int, y: int) -> int:
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
    if tile == TEX_TILE_GLASS:
        return glass(x, y)
    if tile == TEX_TILE_LAMP:
        return lamp(x, y)
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


def write_preview(path: Path, scale: int = 12, gap: int = 8) -> bool:
    """Render a PNG snapshot of the main atlas tiles at base + MIP1 + MIP2.

    The preview is a debug aid for humans, not a build input, so Pillow
    is a soft dependency: if PIL is not importable we just skip the PNG
    and let the .mif generation stand on its own. Returns True when a
    file was written, False otherwise.

    Layout:
      * one column per block type (see PREVIEW_COLUMNS),
      * rows top-to-bottom are base LOD, MIP1, MIP2,
      * each 16x16 tile is upscaled by `scale` with nearest-neighbour,
      * `gap` pixels of background colour separate the cells.

    Palette entries missing from PREVIEW_PALETTE render as bright
    magenta so drift between this table and the real renderer palette
    is impossible to miss.
    """
    try:
        from PIL import Image
    except ImportError:
        print(
            "skip preview: Pillow not installed (pip install Pillow) -- "
            "the .mif was still written"
        )
        return False

    atlas = build_atlas()
    cols = len(PREVIEW_COLUMNS)
    rows = 3
    cell = TILE_SIZE * scale
    width = gap + cols * (cell + gap)
    height = gap + rows * (cell + gap)
    bg = PREVIEW_PALETTE.get(PAL_TRANSPARENT, (0x10, 0x10, 0x18))
    missing = (0xff, 0x00, 0xff)

    image = Image.new("RGB", (width, height), bg)
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

    for col_index, (_label, base_tile, mip1_tile, mip2_tile) in enumerate(
        PREVIEW_COLUMNS
    ):
        cell_x = gap + col_index * (cell + gap)
        for row_index, tile in enumerate((base_tile, mip1_tile, mip2_tile)):
            cell_y = gap + row_index * (cell + gap)
            paint(tile, cell_x, cell_y)

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
