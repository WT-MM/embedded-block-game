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

FACE_LEFT = 2
FACE_RIGHT = 3
FACE_FRONT = 4
FACE_BACK = 5
TORCH_SUPPORT_FLOOR = 8


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


def is_torch(block):
    return block in {
        B["BLOCK_TORCH"],
        B["BLOCK_REDSTONE_TORCH_OFF"],
        B["BLOCK_REDSTONE_TORCH_ON"],
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

    def set(self, wx, wy, wz, name, delay=1, torch_support=None):
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
        if is_repeater(block):
            self.redstone[(cx, cz)][idx] = delay
        elif is_torch(block):
            self.redstone[(cx, cz)][idx] = (
                TORCH_SUPPORT_FLOOR if torch_support is None
                else torch_support
            )
        else:
            self.redstone[(cx, cz)][idx] = 0

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


def place_rising_edge_detector(world, wx, wy, wz):
    """Place the 2x2 core for a compact comparator rising-edge detector.

    Footprint, viewed from above:

        [keepout ] [side repeater]
        [rear tap] [comparator   ] -> pulse out

    The clock bus owns the adjacent cells feeding `rear_input` and the side
    repeater's rear `delay_input`. Keep the rear tap at a weaker dust strength
    than the delayed repeater output, so the comparator turns off once the side
    input arrives.
    """
    world.set(wx, wy, wz, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(wx + 1, wy, wz, "BLOCK_COMPARATOR_EAST_OFF")
    world.set(wx + 1, wy, wz - 1, "BLOCK_REPEATER_SOUTH_OFF")

    return {
        "rear_input": (wx, wy, wz),
        "delay_input": (wx + 1, wy, wz - 2),
        "side_repeater": (wx + 1, wy, wz - 1),
        "output": (wx + 2, wy, wz),
        "footprint": (wx, wz - 1, wx + 1, wz),
    }


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
    world.set(wx + 25, wy, wz, "BLOCK_REDSTONE_TORCH_ON",
              torch_support=FACE_LEFT)
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

DECODER_ROW_SPACING = 3


def place_segment_display(world, wy, decoder_z, bus_z1, segment_bus):
    display_lamps = []
    display = {
        0: {  # A
            "route_z": decoder_z - 14,
            "drive": [("x", -30, -27, decoder_z - 14)],
            "lamps": [(-29, decoder_z - 13), (-28, decoder_z - 13),
                      (-27, decoder_z - 13)],
        },
        1: {  # B
            "route_z": decoder_z - 12,
            "drive": [("z", -18, decoder_z - 12, decoder_z - 10)],
            "lamps": [(-17, decoder_z - 12), (-17, decoder_z - 11),
                      (-17, decoder_z - 10)],
        },
        2: {  # C
            "route_z": decoder_z - 6,
            "drive": [("z", -15, decoder_z - 6, decoder_z - 4)],
            "lamps": [(-14, decoder_z - 6), (-14, decoder_z - 5),
                      (-14, decoder_z - 4)],
        },
        3: {  # D
            "route_z": decoder_z - 2,
            "drive": [("x", -24, -21, decoder_z - 2)],
            "lamps": [(-23, decoder_z - 1), (-22, decoder_z - 1),
                      (-21, decoder_z - 1)],
        },
        4: {  # E
            "route_z": decoder_z - 6,
            "drive": [("z", -39, decoder_z - 6, decoder_z - 4)],
            "lamps": [(-38, decoder_z - 6), (-38, decoder_z - 5),
                      (-38, decoder_z - 4)],
        },
        5: {  # F
            "route_z": decoder_z - 12,
            "drive": [("z", -42, decoder_z - 12, decoder_z - 10)],
            "lamps": [(-43, decoder_z - 12), (-43, decoder_z - 11),
                      (-43, decoder_z - 10)],
        },
        6: {  # G
            "route_z": decoder_z - 8,
            "drive": [("x", -27, -24, decoder_z - 8)],
            "lamps": [(-26, decoder_z - 7), (-25, decoder_z - 7),
                      (-24, decoder_z - 7)],
        },
    }

    for segment, bus_x in enumerate(segment_bus):
        spec = display[segment]
        route_z = spec["route_z"]

        wire_z(world, bus_x, wy, route_z, bus_z1)
        for rz in range(bus_z1 - 2, route_z, -9):
            world.set(bus_x, wy, rz, "BLOCK_REPEATER_OFF")
        for route in spec["drive"]:
            if route[0] == "x":
                _, x0, x1, z = route
                wire_x(world, x0, x1, wy, z)
            else:
                _, x, z0, z1 = route
                wire_z(world, x, wy, z0, z1)

        for x, z in spec["lamps"]:
            world.set(x, wy, z, "BLOCK_LAMP_OFF")
            display_lamps.append((x, wy, z))

    return display_lamps


def place_counter_output_buses(world, cell_x, y, row_z, q_bus, nq_bus,
                               decoder_z, bus_z1):
    source_z = row_z + 14
    decoder_input_z = decoder_z - 4

    world.set(nq_bus, y, row_z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y, row_z + 1, "BLOCK_REPEATER_SOUTH_OFF")
    wire_z(world, nq_bus, y, row_z + 2, row_z + 6)

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
    wire_z(world, nq_bus, y + 4, source_z + 1, decoder_input_z)
    for rz in range(source_z + 7, decoder_input_z, 8):
        world.set(nq_bus, y + 4, rz, "BLOCK_REPEATER_SOUTH_OFF")

    world.set(nq_bus, y + 4, decoder_input_z + 1,
              "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(nq_bus, y + 4, decoder_input_z + 2,
              "BLOCK_REPEATER_SOUTH_OFF")
    wire_z(world, nq_bus, y + 4, decoder_input_z + 3, bus_z1)
    for rz in range(decoder_z + 2, bus_z1, 9):
        world.set(nq_bus, y + 4, rz, "BLOCK_REPEATER_SOUTH_OFF")

    world.set(q_bus + 1, y + 4, decoder_input_z, "BLOCK_STONE")
    world.set(q_bus, y + 4, decoder_input_z, "BLOCK_REDSTONE_TORCH_ON",
              torch_support=FACE_RIGHT)
    world.set(q_bus, y + 4, decoder_input_z + 1,
              "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(q_bus, y + 4, decoder_input_z + 2,
              "BLOCK_REPEATER_SOUTH_OFF")
    wire_z(world, q_bus, y + 4, decoder_input_z + 3, bus_z1)
    for rz in range(decoder_z + 2, bus_z1, 9):
        world.set(q_bus, y + 4, rz, "BLOCK_REPEATER_SOUTH_OFF")


def place_counter_controls(world, y, first_input_x, first_input_z,
                           reset_targets, display_lamps):
    display_max_x = max(x for x, _, _ in display_lamps)
    display_min_z = min(z for _, _, z in display_lamps)
    count_button_x = display_max_x + 1
    count_button_z = display_min_z
    count_bus_z = count_button_z - 1
    count_input_x = first_input_x - 1
    count_repeater = (
        "BLOCK_REPEATER_EAST_OFF"
        if count_button_x <= count_input_x else
        "BLOCK_REPEATER_WEST_OFF"
    )

    world.set(count_button_x, y, count_button_z, "BLOCK_BUTTON")
    repeater_line_x(world, count_button_x, count_input_x, y,
                    count_bus_z, count_repeater, interval=8)
    repeater_line_z(world, count_input_x, y, count_bus_z - 1,
                    first_input_z, "BLOCK_REPEATER_OFF", interval=8)

    for reset_x, reset_z in reset_targets:
        world.set(reset_x, y, reset_z, "BLOCK_BUTTON")

    return {
        "button": (count_button_x, y, count_button_z),
        "reset": reset_targets[-1],
    }


def place_connected_counter_display(world,
                                    wy=5,
                                    decoder_y=7,
                                    decoder_z=36):
    cell_xs = [-8, 19, 46]
    row_zs = [16, 16, 16]
    q_bus = [cell_x + 24 for cell_x in cell_xs]
    nq_bus = [cell_x + 26 for cell_x in cell_xs]
    support_x = min(q_bus) - 3
    row_start_x = support_x + 1
    row_end_x = max(nq_bus) + 1
    bus_z1 = decoder_z + 7 * DECODER_ROW_SPACING + 1
    segment_bus = [support_x - 43, support_x - 31, support_x - 28,
                   support_x - 37, support_x - 52, support_x - 55,
                   support_x - 40]
    segment_bus_min = min(segment_bus)

    for bit, (cell_x, row_z) in enumerate(zip(cell_xs, row_zs)):
        place_t_flip_flop(world, cell_x, wy, row_z, False)

    wire_x(world, nq_bus[0], cell_xs[1] - 1, wy, row_zs[0])
    wire_x(world, nq_bus[1], cell_xs[2] - 1, wy, row_zs[1])

    for cell_x, row_z, qx, nqx in zip(cell_xs, row_zs, q_bus, nq_bus):
        place_counter_output_buses(world, cell_x, wy, row_z,
                                   qx, nqx, decoder_z, bus_z1)

    for value in range(8):
        row_z = decoder_z + value * DECODER_ROW_SPACING
        wire_x(world, row_start_x, row_end_x, decoder_y, row_z)
        literal_x = set(q_bus + nq_bus)
        for rx in range(row_end_x - 5, row_start_x, -8):
            if rx not in literal_x:
                world.set(rx, decoder_y, row_z, "BLOCK_REPEATER_WEST_OFF")

        world.set(support_x, decoder_y, row_z, "BLOCK_STONE")
        world.set(support_x - 1, decoder_y, row_z,
                  "BLOCK_REDSTONE_TORCH_ON",
                  torch_support=FACE_RIGHT)
        wire_x(world, support_x - 2, segment_bus_min - 1,
               decoder_y, row_z)
        rx = support_x - 7
        while rx > segment_bus_min:
            while rx > segment_bus_min and (
                rx in literal_x or rx in segment_bus
            ):
                rx -= 1
            if rx > segment_bus_min:
                world.set(rx, decoder_y, row_z, "BLOCK_REPEATER_WEST_OFF")
            rx -= 6

        for bit in range(3):
            bus_x = nq_bus[bit] if value & (1 << bit) else q_bus[bit]
            world.set(bus_x, decoder_y + 1, row_z, "BLOCK_STONE")

    display_lamps = place_segment_display(world, decoder_y - 2,
                                          decoder_z, bus_z1, segment_bus)
    reset_targets = [(cell_x + 21, row_z - 1)
                     for cell_x, row_z in zip(cell_xs, row_zs)]
    controls = place_counter_controls(world, wy, cell_xs[0], row_zs[0],
                                      reset_targets, display_lamps)

    for value in range(8):
        pattern = SEGMENT_PATTERNS[value]
        row_z = decoder_z + value * DECODER_ROW_SPACING
        for segment, bus_x in enumerate(segment_bus):
            if pattern & (1 << segment):
                world.set(bus_x, decoder_y - 1, row_z, "BLOCK_STONE")

    return {
        "button": controls["button"],
        "reset": controls["reset"],
        "bit_lamps": [],
        "display_lamps": display_lamps,
    }


def place_redstone_clock(world):
    y = 5
    z = -24
    world.set(2, y, z, "BLOCK_STONE")
    world.set(3, y, z, "BLOCK_REDSTONE_TORCH_ON",
              torch_support=FACE_LEFT)
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
