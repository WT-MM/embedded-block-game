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


def noise(x: int, y: int, seed: int) -> int:
    value = (x * 97) ^ (y * 57) ^ (seed * 131)
    value = (value * 1103515245 + 12345) & 0x7FFFFFFF
    return value & 0xFF


def choose_from_noise(n: int, base: int, dark: int, light: int) -> int:
    if n < 36:
        return dark
    if n > 212:
        return light
    return base


def grass_top(x: int, y: int) -> int:
    n = noise(x, y, 1)
    value = choose_from_noise(n, PAL_GRASS_TOP, PAL_GRASS_DARK, PAL_GRASS_LIGHT)
    if ((x * 3 + y * 5) & 7) == 0:
        value = PAL_GRASS_DARK
    elif ((x * 7 + y * 3) & 15) == 1:
        value = PAL_GRASS_LIGHT
    return value


def dirt(x: int, y: int) -> int:
    n = noise(x, y, 2)
    value = choose_from_noise(n, PAL_DIRT, PAL_DIRT_DARK, PAL_DIRT_LIGHT)
    if ((x + y * 2) % 7) == 0:
        value = PAL_DIRT_DARK
    elif ((x * 5 + y) % 11) == 2:
        value = PAL_DIRT_LIGHT
    return value


def grass_side(x: int, y: int) -> int:
    turf_height = 3 + ((x * 5 + 3) & 1)
    if ((x * 3 + 1) % 7) == 0:
        turf_height += 1

    if y < turf_height:
        n = noise(x, y, 3)
        value = choose_from_noise(n, PAL_GRASS_SIDE, PAL_GRASS_DARK, PAL_GRASS_LIGHT)
        if y == turf_height - 1 and ((x + y * 2) % 5) == 0:
            value = PAL_GRASS_DARK
        return value

    return dirt(x, y)


def stone(x: int, y: int) -> int:
    coarse = noise(x >> 1, y >> 1, 4)
    large = noise(x >> 2, y >> 2, 7)

    if coarse < 52:
        value = PAL_STONE_DARK
    elif coarse > 214:
        value = PAL_STONE_LIGHT
    else:
        value = PAL_STONE

    if large > 228:
        value = PAL_STONE_LIGHT
    elif large < 18:
        value = PAL_STONE_DARK

    if ((x + y + (large >> 5)) % 11) == 0:
        value = PAL_STONE_DARK
    elif ((x - y + (coarse >> 4)) % 13) == 0:
        value = PAL_STONE_LIGHT

    return value


def wood_side(x: int, y: int) -> int:
    stripe = (x // 3) & 1
    value = PAL_WOOD if stripe else PAL_WOOD_GRAIN
    if x in (2, 7, 11, 14):
        value = PAL_WOOD_DARK
    elif ((y * 5 + x) % 9) == 0:
        value = PAL_WOOD_GRAIN
    return value


def wood_top(x: int, y: int) -> int:
    dx = x - 7.5
    dy = y - 7.5
    radius = int((dx * dx + dy * dy) ** 0.5 * 1.35)
    ring = radius % 4

    if ring == 0:
        value = PAL_WOOD_DARK
    elif ring == 1:
        value = PAL_WOOD_GRAIN
    else:
        value = PAL_WOOD_TOP

    if abs(x - 7) <= 1 and abs(y - 7) <= 1:
        value = PAL_WOOD_DARK
    elif ((x * 9 + y * 5) % 13) == 0:
        value = PAL_WOOD_GRAIN
    return value


def crosshair(x: int, y: int) -> int:
    if (x == 7 and 4 <= y <= 11) or (y == 7 and 4 <= x <= 11):
        return PAL_WHITE
    if (x == 8 and 5 <= y <= 10) or (y == 8 and 5 <= x <= 10):
        return PAL_WHITE
    return PAL_TRANSPARENT


def texel(tile: int, x: int, y: int) -> int:
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
