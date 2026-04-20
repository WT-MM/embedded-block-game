from __future__ import annotations

from pathlib import Path

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
TEX_TILE_GRASS_TOP_MIP1 = 16
TEX_TILE_GRASS_SIDE_MIP1 = 17
TEX_TILE_DIRT_MIP1 = 18
TEX_TILE_STONE_MIP1 = 19
TEX_TILE_WOOD_SIDE_MIP1 = 20
TEX_TILE_WOOD_TOP_MIP1 = 21
TEX_TILE_GRASS_TOP_MIP2 = 24
TEX_TILE_GRASS_SIDE_MIP2 = 25
TEX_TILE_DIRT_MIP2 = 26
TEX_TILE_STONE_MIP2 = 27
TEX_TILE_WOOD_SIDE_MIP2 = 28
TEX_TILE_WOOD_TOP_MIP2 = 29
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


MIP1_TILES = {
    TEX_TILE_GRASS_TOP_MIP1: TEX_TILE_GRASS_TOP,
    TEX_TILE_GRASS_SIDE_MIP1: TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT_MIP1: TEX_TILE_DIRT,
    TEX_TILE_STONE_MIP1: TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE_MIP1: TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP_MIP1: TEX_TILE_WOOD_TOP,
}

MIP2_TILES = {
    TEX_TILE_GRASS_TOP_MIP2: TEX_TILE_GRASS_TOP,
    TEX_TILE_GRASS_SIDE_MIP2: TEX_TILE_GRASS_SIDE,
    TEX_TILE_DIRT_MIP2: TEX_TILE_DIRT,
    TEX_TILE_STONE_MIP2: TEX_TILE_STONE,
    TEX_TILE_WOOD_SIDE_MIP2: TEX_TILE_WOOD_SIDE,
    TEX_TILE_WOOD_TOP_MIP2: TEX_TILE_WOOD_TOP,
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


def write_hex(path: Path) -> None:
    atlas = build_atlas()
    with path.open("w", encoding="ascii", newline="\n") as handle:
        for value in atlas:
            handle.write(f"{value:02x}\n")


def main() -> int:
    out_path = Path(__file__).with_name("textures.hex")
    write_hex(out_path)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
