#!/usr/bin/env python3
import pathlib
import re
import struct


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORLD_ROOT = pathlib.Path(__file__).resolve().parent
CHUNK_ROOT = WORLD_ROOT / "chunks"
BLOCK_TYPES = ROOT / "sw" / "block_types.h"

CHUNK_SIZE = 16
CHUNK_HEIGHT = 32
CHUNK_MIN = -4
CHUNK_MAX = 4
BLOCK_COUNT = CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE

WORLD_SEED = 0x4840C0DE
STONE_TRIES_PER_CHUNK = 12


def load_block_ids():
    text = BLOCK_TYPES.read_text()
    match = re.search(r"typedef enum \{(.*?)NUM_BLOCK_TYPES", text, re.S)
    if not match:
        raise RuntimeError("could not find BlockID enum")

    ids = {}
    value = 0
    for raw in match.group(1).splitlines():
        line = raw.split("/*")[0].strip().rstrip(",")
        if not line or not line.startswith("BLOCK_"):
            continue
        if "=" in line:
            name, expr = [part.strip() for part in line.split("=", 1)]
            value = int(expr, 0)
        else:
            name = line
        ids[name] = value
        value += 1
    return ids


B = load_block_ids()


def floor_div(value, divisor):
    q = value // divisor
    return q


def block_index(x, y, z):
    return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x


def is_repeater(block):
    return block in {
        B["BLOCK_REPEATER_OFF"],
        B["BLOCK_REPEATER_ON"],
        B["BLOCK_REPEATER_EAST_OFF"],
        B["BLOCK_REPEATER_SOUTH_OFF"],
        B["BLOCK_REPEATER_WEST_OFF"],
        B["BLOCK_REPEATER_EAST_ON"],
        B["BLOCK_REPEATER_SOUTH_ON"],
        B["BLOCK_REPEATER_WEST_ON"],
    }


class LabWorld:
    def __init__(self):
        self.blocks = {}
        self.redstone = {}
        for cx in range(CHUNK_MIN, CHUNK_MAX + 1):
            for cz in range(CHUNK_MIN, CHUNK_MAX + 1):
                block_data = bytearray(BLOCK_COUNT)
                redstone_data = bytearray(BLOCK_COUNT)
                for y in range(CHUNK_HEIGHT):
                    for z in range(CHUNK_SIZE):
                        for x in range(CHUNK_SIZE):
                            idx = block_index(x, y, z)
                            if y <= 2:
                                block_data[idx] = B["BLOCK_STONE"]
                            elif y == 3:
                                block_data[idx] = B["BLOCK_DIRT"]
                            elif y == 4:
                                block_data[idx] = B["BLOCK_GRASS"]
                self.blocks[(cx, cz)] = block_data
                self.redstone[(cx, cz)] = redstone_data

    def set(self, wx, wy, wz, name, delay=1):
        if wy < 0 or wy >= CHUNK_HEIGHT:
            raise ValueError(f"y out of range: {wy}")
        cx = floor_div(wx, CHUNK_SIZE)
        cz = floor_div(wz, CHUNK_SIZE)
        if (cx, cz) not in self.blocks:
            raise ValueError(f"coordinate outside lab chunks: {(wx, wy, wz)}")
        lx = wx - cx * CHUNK_SIZE
        lz = wz - cz * CHUNK_SIZE
        idx = block_index(lx, wy, lz)
        block = B[name]
        self.blocks[(cx, cz)][idx] = block
        self.redstone[(cx, cz)][idx] = delay if is_repeater(block) else 0

    def get(self, wx, wy, wz):
        cx = floor_div(wx, CHUNK_SIZE)
        cz = floor_div(wz, CHUNK_SIZE)
        if (cx, cz) not in self.blocks or wy < 0 or wy >= CHUNK_HEIGHT:
            return B["BLOCK_AIR"]
        lx = wx - cx * CHUNK_SIZE
        lz = wz - cz * CHUNK_SIZE
        return self.blocks[(cx, cz)][block_index(lx, wy, lz)]

    def write(self):
        CHUNK_ROOT.mkdir(parents=True, exist_ok=True)
        for old in CHUNK_ROOT.glob("*.chk"):
            old.unlink()
        for (cx, cz), block_data in sorted(self.blocks.items()):
            header = struct.pack(
                "<4siiiHHI",
                b"VCHK",
                3,
                cx,
                cz,
                CHUNK_SIZE,
                CHUNK_HEIGHT,
                BLOCK_COUNT,
            )
            path = CHUNK_ROOT / f"{cx}_{cz}.chk"
            path.write_bytes(
                header +
                bytes(block_data) +
                bytes(BLOCK_COUNT) +
                bytes(self.redstone[(cx, cz)])
            )

        meta = struct.pack(
            "<4sIIiHHI",
            b"VWLD",
            2,
            WORLD_SEED,
            STONE_TRIES_PER_CHUNK,
            CHUNK_SIZE,
            CHUNK_HEIGHT,
            0,
        )
        (WORLD_ROOT / "world.meta").write_bytes(meta)


def wire_x(world, x0, x1, y, z):
    step = 1 if x0 <= x1 else -1
    for x in range(x0, x1 + step, step):
        world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")


def wire_z(world, x, y, z0, z1):
    step = 1 if z0 <= z1 else -1
    for z in range(z0, z1 + step, step):
        world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")


def place_wide_comparator_latch(world, wx, wy, wz):
    feedback = [
        (1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (3, 3),
        (2, 3), (1, 3), (0, 3), (-1, 3), (-2, 3),
        (-2, 2), (-2, 1), (-2, 0), (-1, 0),
    ]
    world.set(wx, wy, wz, "BLOCK_COMPARATOR_EAST_OFF")
    for dx, dz in feedback:
        world.set(wx + dx, wy, wz + dz, "BLOCK_REDSTONE_WIRE_UNCONNECTED")


def place_t_flip_flop(world, wx, wy, wz, button_input):
    world.set(wx, wy, wz,
              "BLOCK_BUTTON" if button_input else "BLOCK_REPEATER_EAST_OFF")
    world.set(wx + 1, wy, wz, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 2, wy, wz, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 3, wy, wz, "BLOCK_COMPARATOR_EAST_OFF")
    world.set(wx + 3, wy, wz - 1, "BLOCK_REPEATER_SOUTH_OFF")
    world.set(wx + 1, wy, wz - 1, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 1, wy, wz - 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 2, wy, wz - 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 3, wy, wz - 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")

    wire_x(world, wx + 4, wx + 10, wy, wz)
    world.set(wx + 11, wy, wz, "BLOCK_REPEATER_EAST_OFF")
    wire_x(world, wx + 12, wx + 16, wy, wz)
    wire_z(world, wx + 16, wy, wz - 1, wz - 4)
    wire_x(world, wx + 17, wx + 20, wy, wz - 4)

    world.set(wx + 17, wy, wz, "BLOCK_COMPARATOR_EAST_OFF")
    world.set(wx + 20, wy, wz - 3, "BLOCK_COMPARATOR_SOUTH_OFF")
    place_wide_comparator_latch(world, wx + 20, wy, wz)

    world.set(wx + 17, wy, wz + 1, "BLOCK_REPEATER_OFF")
    world.set(wx + 17, wy, wz + 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 18, wy, wz + 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 20, wy, wz - 2, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 20, wy, wz - 1, "BLOCK_REDSTONE_WIRE_UNCONNECTED")

    world.set(wx + 24, wy, wz, "BLOCK_STONE")
    world.set(wx + 25, wy, wz, "BLOCK_REDSTONE_TORCH_ON")
    wire_z(world, wx + 25, wy, wz - 1, wz - 3)
    wire_x(world, wx + 22, wx + 24, wy, wz - 3)
    world.set(wx + 21, wy, wz - 3, "BLOCK_REPEATER_WEST_OFF")


SEG_A = 1 << 0
SEG_B = 1 << 1
SEG_C = 1 << 2
SEG_D = 1 << 3
SEG_E = 1 << 4
SEG_F = 1 << 5
SEG_G = 1 << 6


SEGMENT_PATTERNS = [
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C,
]


def place_three_bit_counter(world, wx=-48, wy=5, wz=8, bit_stride=30):
    for bit in range(3):
        cell_x = wx + bit * bit_stride
        place_t_flip_flop(world, cell_x, wy, wz, bit == 0)
        world.set(cell_x + 21, wy, wz - 1, "BLOCK_BUTTON")

    wire_x(world, wx + 26, wx + bit_stride - 1, wy, wz)
    wire_x(world, wx + bit_stride + 26, wx + 2 * bit_stride - 1, wy, wz)

    for bit in range(3):
        cell_x = wx + bit * bit_stride
        wire_z(world, cell_x + 20, wy, wz + 4, wz + 6)
        world.set(cell_x + 20, wy, wz + 7, "BLOCK_LAMP_OFF")

    return {
        "button": (wx, wy, wz),
        "resets": [(wx + bit * bit_stride + 21, wy, wz - 1)
                   for bit in range(3)],
    }


def place_decoder_demo(world, wx=18, wy=7, wz=16):
    q_bus = [wx + bit * 5 for bit in range(3)]
    nq_bus = [wx + bit * 5 + 2 for bit in range(3)]
    support_x = wx + 15
    row_start_x = wx - 1
    row_end_x = support_x - 1
    source_z = wz - 2
    bus_z0 = wz - 1
    bus_z1 = wz + 7 * 3 + 1
    segment_bus = [support_x + 4 + segment * 2 for segment in range(7)]
    lamp_z = wz + 7 * 3 + 4

    for bit in range(3):
        wire_z(world, q_bus[bit], wy + 2, bus_z0, bus_z1)
        wire_z(world, nq_bus[bit], wy + 2, bus_z0, bus_z1)
        for rz in (bus_z0 + 3, bus_z0 + 12, bus_z0 + 21):
            if rz < bus_z1:
                world.set(q_bus[bit], wy + 2, rz,
                          "BLOCK_REPEATER_SOUTH_OFF")
                world.set(nq_bus[bit], wy + 2, rz,
                          "BLOCK_REPEATER_SOUTH_OFF")
        world.set(q_bus[bit], wy + 2, source_z, "BLOCK_AIR")
        world.set(nq_bus[bit], wy + 2, source_z, "BLOCK_REDSTONE_BLOCK")

    for value in range(8):
        row_z = wz + value * 3
        wire_x(world, row_start_x, row_end_x, wy, row_z)
        for rx in (wx + 4, wx + 9):
            if rx not in q_bus and rx not in nq_bus:
                world.set(rx, wy, row_z, "BLOCK_REPEATER_EAST_OFF")
        world.set(support_x, wy, row_z, "BLOCK_STONE")
        world.set(support_x + 1, wy, row_z, "BLOCK_REDSTONE_TORCH_ON")
        wire_x(world, support_x + 2, segment_bus[-1] + 1, wy, row_z)
        world.set(support_x + 5, wy, row_z, "BLOCK_REPEATER_EAST_OFF")
        world.set(support_x + 11, wy, row_z, "BLOCK_REPEATER_EAST_OFF")

        for bit in range(3):
            bus_x = nq_bus[bit] if value & (1 << bit) else q_bus[bit]
            world.set(bus_x, wy + 1, row_z, "BLOCK_STONE")

    for segment, bus_x in enumerate(segment_bus):
        wire_z(world, bus_x, wy - 2, wz, lamp_z - 1)
        for rz in (wz + 2, wz + 11, wz + 20):
            world.set(bus_x, wy - 2, rz, "BLOCK_REPEATER_SOUTH_OFF")
        world.set(bus_x, wy - 2, lamp_z, "BLOCK_LAMP_OFF")

    for value, pattern in enumerate(SEGMENT_PATTERNS):
        row_z = wz + value * 3
        for segment, bus_x in enumerate(segment_bus):
            if pattern & (1 << segment):
                world.set(bus_x, wy - 1, row_z, "BLOCK_STONE")

    return {
        "sources": [(q_bus[bit], wy + 2, source_z,
                     nq_bus[bit], wy + 2, source_z)
                    for bit in range(3)],
        "lamps": [(x, wy - 2, lamp_z) for x in segment_bus],
    }


def place_clock_and_button_demo(world):
    y = 5
    z = -24
    world.set(2, y, z, "BLOCK_STONE")
    world.set(3, y, z, "BLOCK_REDSTONE_TORCH_ON")
    wire_x(world, 4, 7, y, z)
    world.set(8, y, z, "BLOCK_REPEATER_EAST_OFF", delay=2)
    wire_x(world, 9, 11, y, z)
    world.set(12, y, z, "BLOCK_REPEATER_EAST_OFF", delay=4)
    wire_z(world, 12, y, z + 1, z + 4)
    wire_x(world, 4, 11, y, z + 4)
    wire_z(world, 4, y, z + 3, z + 1)

    world.set(26, y, z, "BLOCK_BUTTON")
    wire_x(world, 27, 31, y, z)
    world.set(32, y, z, "BLOCK_REPEATER_EAST_OFF", delay=1)
    world.set(34, y, z, "BLOCK_REPEATER_EAST_OFF", delay=4)
    wire_x(world, 35, 38, y, z)
    world.set(39, y, z, "BLOCK_LAMP_OFF")


def place_comparator_demo(world):
    y = 5
    z = -36
    world.set(-42, y, z, "BLOCK_LEVER_ON")
    wire_x(world, -41, -37, y, z)
    world.set(-36, y, z, "BLOCK_COMPARATOR_EAST_OFF")
    wire_x(world, -35, -31, y, z)
    world.set(-30, y, z, "BLOCK_LAMP_OFF")

    world.set(-42, y, z + 6, "BLOCK_LEVER_ON")
    wire_x(world, -41, -37, y, z + 6)
    world.set(-36, y, z + 6, "BLOCK_COMPARATOR_EAST_OFF")
    wire_x(world, -35, -31, y, z + 6)
    world.set(-30, y, z + 6, "BLOCK_LAMP_OFF")
    world.set(-36, y, z + 4, "BLOCK_LEVER_ON")
    wire_z(world, -36, y, z + 5, z + 5)


def place_standalone_tff(world):
    y = 5
    wx = -44
    wz = 28
    place_t_flip_flop(world, wx, y, wz, True)
    world.set(wx + 21, y, wz - 1, "BLOCK_BUTTON")
    wire_x(world, wx + 21, wx + 24, y, wz + 4)
    world.set(wx + 25, y, wz + 4, "BLOCK_LAMP_OFF")


def main():
    world = LabWorld()
    place_comparator_demo(world)
    place_clock_and_button_demo(world)
    place_standalone_tff(world)
    place_three_bit_counter(world)
    place_decoder_demo(world)
    world.write()


if __name__ == "__main__":
    main()
