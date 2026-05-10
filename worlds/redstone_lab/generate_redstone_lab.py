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
                                block_data[idx] = B["BLOCK_PLANKS"]
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


def fill_floor(world, x0, x1, z0, z1, material):
    xmin, xmax = sorted((x0, x1))
    zmin, zmax = sorted((z0, z1))
    for x in range(xmin, xmax + 1):
        for z in range(zmin, zmax + 1):
            world.set(x, 4, z, material)


def repeater_line_x(world, x0, x1, y, z, repeater_name, interval=10):
    step = 1 if x0 <= x1 else -1
    distance = 0
    for x in range(x0, x1 + step, step):
        if distance > 0 and x != x1 and distance % interval == 0:
            world.set(x, y, z, repeater_name)
        else:
            world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
        distance += 1


def repeater_line_z(world, x, y, z0, z1, repeater_name, interval=10):
    step = 1 if z0 <= z1 else -1
    distance = 0
    for z in range(z0, z1 + step, step):
        if distance > 0 and z != z1 and distance % interval == 0:
            world.set(x, y, z, repeater_name)
        else:
            world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
        distance += 1


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

DECODER_ROW_SPACING = 2


DISPLAY_SEGMENT_BUSES = [
    58,  # A: top
    66,  # B: upper right
    68,  # C: lower right
    70,  # D: bottom
    52,  # E: lower left
    54,  # F: upper left
    50,  # G: middle
]


DISPLAY_SEGMENT_DRIVERS = [
    [(x, 63) for x in range(57, 64)],
    [(65, z) for z in range(66, 69)],
    [(65, z) for z in range(72, 75)],
    [(x, 75) for x in range(57, 64)],
    [(55, z) for z in range(72, 75)],
    [(55, z) for z in range(66, 69)],
    [(x, 70) for x in range(57, 64)],
]


DISPLAY_SEGMENT_LAMPS = [
    [(x, 64) for x in range(57, 64)],
    [(64, z) for z in range(66, 69)],
    [(64, z) for z in range(72, 75)],
    [(x, 76) for x in range(57, 64)],
    [(56, z) for z in range(72, 75)],
    [(56, z) for z in range(66, 69)],
    [(x, 71) for x in range(57, 64)],
]


DISPLAY_SEGMENT_REPEATERS = [
    [],
    [],
    [],
    [(66, 75, "BLOCK_REPEATER_WEST_OFF"),
     (61, 75, "BLOCK_REPEATER_WEST_OFF")],
    [],
    [],
    [],
]


DISPLAY_SEGMENT_BUS_REPEATERS = [
    [],
    [],
    [],
    [],
    [],
    [],
    [],
]


def place_segment_display(world, wy, decoder_z, bus_z1, segment_bus):
    display_lamps = []

    for segment, bus_x in enumerate(segment_bus):
        max_driver_z = max(z for _, z in DISPLAY_SEGMENT_DRIVERS[segment])
        bus_end_z = max(max_driver_z, bus_z1)

        wire_z(world, bus_x, wy, decoder_z, bus_end_z)
        for rz in DISPLAY_SEGMENT_BUS_REPEATERS[segment]:
            if decoder_z < rz < bus_end_z:
                world.set(bus_x, wy, rz, "BLOCK_REPEATER_SOUTH_OFF")

        for dx, dz in DISPLAY_SEGMENT_DRIVERS[segment]:
            wire_x(world, bus_x, dx, wy, dz)
        for rx, rz, repeater in DISPLAY_SEGMENT_REPEATERS[segment]:
            world.set(rx, wy, rz, repeater)
        for lx, lz in DISPLAY_SEGMENT_LAMPS[segment]:
            world.set(lx, wy, lz, "BLOCK_LAMP_OFF")
            display_lamps.append((lx, wy, lz))

    return display_lamps


def place_counter_output_buses(world, cell_x, y, row_z, q_bus, nq_bus,
                               decoder_z, bus_z1):
    source_z = row_z + 14

    world.set(nq_bus, y, row_z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y, row_z + 1, "BLOCK_REPEATER_SOUTH_OFF")
    wire_z(world, nq_bus, y, row_z + 2, row_z + 6)
    world.set(nq_bus - 1, y, row_z + 6, "BLOCK_LAMP_OFF")

    world.set(nq_bus, y, row_z + 7, "BLOCK_REPEATER_SOUTH_OFF")
    world.set(nq_bus, y, row_z + 8, "BLOCK_STONE")
    world.set(nq_bus, y + 1, row_z + 8, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y + 1, row_z + 9, "BLOCK_REPEATER_SOUTH_OFF")
    world.set(nq_bus, y + 1, row_z + 10, "BLOCK_STONE")
    world.set(nq_bus, y + 2, row_z + 10, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y + 2, row_z + 11, "BLOCK_REPEATER_SOUTH_OFF")
    world.set(nq_bus, y + 2, row_z + 12, "BLOCK_STONE")
    world.set(nq_bus, y + 3, row_z + 12, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y + 3, row_z + 13, "BLOCK_REPEATER_SOUTH_OFF")
    world.set(nq_bus, y + 3, row_z + 14, "BLOCK_STONE")
    world.set(nq_bus, y + 4, source_z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    wire_z(world, nq_bus, y + 4, source_z + 1, bus_z1)
    for rz in range(source_z + 7, bus_z1, 8):
        world.set(nq_bus, y + 4, rz, "BLOCK_REPEATER_SOUTH_OFF")

    world.set(q_bus + 1, y + 4, source_z, "BLOCK_STONE")
    world.set(q_bus, y + 4, source_z, "BLOCK_REDSTONE_TORCH_ON")
    wire_z(world, q_bus, y + 4, source_z + 1, bus_z1)
    for rz in range(source_z + 7, bus_z1, 8):
        world.set(q_bus, y + 4, rz, "BLOCK_REPEATER_SOUTH_OFF")


def route_counter_carry(world, y, source_x, source_z,
                        input_x, input_z):
    branch_x = source_x + 2
    mid_z = source_z + (input_z - source_z) // 2

    wire_x(world, source_x, branch_x, y, source_z)
    repeater_line_z(world, branch_x, y, source_z + 1, mid_z,
                    "BLOCK_REPEATER_SOUTH_OFF", interval=6)
    repeater_line_x(world, branch_x, input_x - 1, y, mid_z,
                    "BLOCK_REPEATER_WEST_OFF", interval=6)
    repeater_line_z(world, input_x - 1, y, mid_z + 1, input_z,
                    "BLOCK_REPEATER_SOUTH_OFF", interval=6)


def place_counter_controls(world, y, first_input_x, first_input_z,
                           reset_targets):
    count_x = first_input_x - 1
    world.set(count_x, y - 1, first_input_z, "BLOCK_GOLD_BLOCK")
    world.set(count_x, y, first_input_z, "BLOCK_BUTTON")

    return {
        "button": (count_x, y, first_input_z),
        "reset": reset_targets[-1],
    }


def place_connected_counter_display(world,
                                    wy=5,
                                    decoder_y=7,
                                    decoder_z=60):
    cell_xs = [4, 12, 20]
    row_zs = [8, 26, 44]
    q_bus = [cell_x + 24 for cell_x in cell_xs]
    nq_bus = [cell_x + 26 for cell_x in cell_xs]
    support_x = max(nq_bus) + 2
    row_start_x = min(q_bus) - 1
    row_end_x = support_x - 1
    bus_z1 = decoder_z + 7 * DECODER_ROW_SPACING + 1
    segment_bus = DISPLAY_SEGMENT_BUSES
    segment_bus_max = max(segment_bus)

    for cell_x, row_z in zip(cell_xs, row_zs):
        place_t_flip_flop(world, cell_x, wy, row_z, False)
    world.set(cell_xs[0], wy, row_zs[0],
              "BLOCK_REDSTONE_WIRE_UNCONNECTED")

    route_counter_carry(world, wy, nq_bus[0], row_zs[0],
                        cell_xs[1], row_zs[1])
    route_counter_carry(world, wy, nq_bus[1], row_zs[1],
                        cell_xs[2], row_zs[2])

    for cell_x, row_z, qx, nqx in zip(cell_xs, row_zs, q_bus, nq_bus):
        place_counter_output_buses(world, cell_x, wy, row_z,
                                   qx, nqx, decoder_z, bus_z1)

    for value in range(8):
        row_z = decoder_z + value * DECODER_ROW_SPACING
        wire_x(world, row_start_x, row_end_x, decoder_y, row_z)
        literal_x = set(q_bus + nq_bus)
        for rx in range(row_start_x + 5, row_end_x, 8):
            if rx not in literal_x:
                world.set(rx, decoder_y, row_z, "BLOCK_REPEATER_EAST_OFF")

        world.set(support_x, decoder_y, row_z, "BLOCK_STONE")
        world.set(support_x + 1, decoder_y, row_z,
                  "BLOCK_REDSTONE_TORCH_ON")
        wire_x(world, support_x + 2, segment_bus_max + 1,
               decoder_y, row_z)
        for rx in (support_x + 5, support_x + 11,
                   support_x + 19, support_x + 27):
            if rx < segment_bus_max:
                world.set(rx, decoder_y, row_z,
                          "BLOCK_REPEATER_EAST_OFF")

        for bit in range(3):
            bus_x = nq_bus[bit] if value & (1 << bit) else q_bus[bit]
            world.set(bus_x, decoder_y + 1, row_z, "BLOCK_STONE")

    display_lamps = place_segment_display(world, decoder_y - 2,
                                          decoder_z, bus_z1, segment_bus)
    reset_targets = [(cell_x + 21, row_z - 1)
                     for cell_x, row_z in zip(cell_xs, row_zs)]
    controls = place_counter_controls(world, wy, cell_xs[0], row_zs[0],
                                      reset_targets)

    for value in range(8):
        pattern = SEGMENT_PATTERNS[(value - 6) & 7]
        row_z = decoder_z + value * DECODER_ROW_SPACING
        for segment, bus_x in enumerate(segment_bus):
            if pattern & (1 << segment):
                world.set(bus_x, decoder_y - 1, row_z, "BLOCK_STONE")

    return {
        "button": controls["button"],
        "reset": controls["reset"],
        "bit_lamps": [(nqx - 1, wy, row_z + 6)
                      for nqx, row_z in zip(nq_bus, row_zs)],
        "display_lamps": display_lamps,
    }


def place_redstone_clock(world):
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


def place_latch_demo(world):
    y = 5
    wx = -42
    wz = -18
    feedback = [
        (1, 0), (2, 0), (2, 1), (2, 2), (1, 2), (0, 2),
        (-1, 2), (-2, 2), (-2, 1), (-2, 0), (-1, 0),
    ]

    world.set(wx - 3, y, wz, "BLOCK_BUTTON")
    world.set(wx, y, wz - 1, "BLOCK_BUTTON")
    world.set(wx, y, wz, "BLOCK_COMPARATOR_EAST_OFF")
    for dx, dz in feedback:
        world.set(wx + dx, y, wz + dz, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 3, y, wz, "BLOCK_LAMP_OFF")


def place_section_floors(world):
    fill_floor(world, -46, -28, -39, -28, "BLOCK_STONE")
    fill_floor(world, -3, 16, -28, -18, "BLOCK_WOOD")
    fill_floor(world, -48, -34, -23, -13, "BLOCK_COBBLESTONE")
    fill_floor(world, -48, 39, 3, 20, "BLOCK_PLANKS")
    fill_floor(world, -24, 76, 21, 78, "BLOCK_SANDSTONE")


def main():
    world = LabWorld()
    place_section_floors(world)
    place_comparator_demo(world)
    place_redstone_clock(world)
    place_latch_demo(world)
    place_connected_counter_display(world)
    world.write()


if __name__ == "__main__":
    main()
