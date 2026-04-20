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


def noise(x: int, y: int, seed: int) -> int:
    value = (x * 97) ^ (y * 57) ^ (seed * 131)
    value = (value * 1103515245 + 12345) & 0x7FFFFFFF
    return value & 0xFF


def grass_top(x: int, y: int) -> int:
    # Sparse 2x2 clumps of shade variation over a mostly uniform green field.
    clump = noise(x >> 1, y >> 1, 1)
    if clump < 48:
        return PAL_GRASS_DARK
    if clump > 220:
        return PAL_GRASS_LIGHT
    # Rare single-pixel blade highlights.
    if noise(x, y, 11) > 245:
        return PAL_GRASS_LIGHT
    return PAL_GRASS_TOP


def dirt(x: int, y: int) -> int:
    # Mostly uniform dirt with a few 2x2 shade clumps and sparse pebbles.
    clump = noise(x >> 1, y >> 1, 2)
    if clump < 40:
        return PAL_DIRT_DARK
    if clump > 220:
        return PAL_DIRT_LIGHT
    if noise(x, y, 12) > 248:
        return PAL_DIRT_DARK
    return PAL_DIRT


def grass_side(x: int, y: int) -> int:
    # Always at least 3 rows of grass at the top; some columns drip one or two
    # pixels further into the dirt for a jagged edge. Never carve into the top.
    col = noise(x, 0, 3)
    drip = 3
    if (col & 3) == 0:
        drip = 4
    if (col & 15) == 1:
        drip = 5
    if y < drip:
        clump = noise(x >> 1, y, 8)
        if clump < 48:
            return PAL_GRASS_DARK
        if clump > 220:
            return PAL_GRASS_LIGHT
        return PAL_GRASS_SIDE
    return dirt(x, y)


def stone(x: int, y: int) -> int:
    # Blend fine per-pixel speckle with a coarser 2x2 mottle so neither
    # dimension dominates — keeps the blobs smaller than the tile.
    fine = noise(x, y, 4)
    coarse = noise(x >> 1, y >> 1, 17)
    mix = (fine + coarse) >> 1
    if mix < 70:
        base = PAL_STONE_DARK
    elif mix > 195:
        base = PAL_STONE_LIGHT
    else:
        base = PAL_STONE
    # Sparse crack accents along a couple of shallow diagonals.
    if ((x + 2 * y) % 19) == 4 and noise(x, y, 15) > 170:
        return PAL_STONE_DARK
    return base


def wood_side(x: int, y: int) -> int:
    # Vertical bark with darker columns at the edges and one offset knot.
    if x == 0 or x == 15:
        return PAL_WOOD_DARK
    if x in (1, 14) and (y & 3) != 0:
        return PAL_WOOD_GRAIN
    # Knot
    dx = x - 6
    dy = y - 10
    r2 = dx * dx + dy * dy
    if r2 <= 1:
        return PAL_WOOD_DARK
    if r2 <= 4:
        return PAL_WOOD_GRAIN
    # Occasional vertical grain streaks — one out of every few columns.
    if noise(x, 0, 5) < 40 and (y & 1) == 0:
        return PAL_WOOD_GRAIN
    return PAL_WOOD


def wood_top(x: int, y: int) -> int:
    # Clean concentric rings around the center.
    dx = x - 7.5
    dy = y - 7.5
    r = (dx * dx + dy * dy) ** 0.5
    if r < 1.2:
        return PAL_WOOD_DARK
    ring = int(r * 1.1) % 3
    if ring == 0:
        return PAL_WOOD_GRAIN
    if ring == 1:
        return PAL_WOOD_TOP
    return PAL_WOOD


def crosshair(x: int, y: int) -> int:
    if (x == 7 and 4 <= y <= 11) or (y == 7 and 4 <= x <= 11):
        return PAL_WHITE
    if (x == 8 and 5 <= y <= 10) or (y == 8 and 5 <= x <= 10):
        return PAL_WHITE
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
