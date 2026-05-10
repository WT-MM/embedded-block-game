#!/usr/bin/env python3
"""Generate the compact "redstone_lab_2" world.

Designed to fit comfortably inside a 3-chunk render radius around the player
spawn. Hosts four small, *working* circuits laid out around the spawn point at
(0, ~5, -1.5):

    * Memory cell      -- cross-coupled-torch SR latch (west of spawn).
    * Comparator demo  -- side input vetoes a decayed rear (east of spawn).
    * Redstone clock   -- torch+repeater oscillator gated by a button-driven
                          NOT cell (north-west of spawn).
    * 7-seg counter    -- 1-bit SR-latched display showing "0" or "1" with an
                          increment (set) and reset button (north-east of
                          spawn).

Floor is plain planks at y=4. All circuit blocks sit at y=5; buttons sit at
y=6 directly on top of the stone they pulse.
"""
import pathlib
import re
import struct


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORLD_ROOT = pathlib.Path(__file__).resolve().parent
CHUNK_ROOT = WORLD_ROOT / "chunks"
BLOCK_TYPES = ROOT / "sw" / "block_types.h"

CHUNK_SIZE = 16
CHUNK_HEIGHT = 32
# A 7x7 chunk patch around the spawn chunk (0, -1) so even at the edges the
# player only sees flat plank floor, not procedurally generated terrain.
CHUNK_X_MIN = -3
CHUNK_X_MAX = 3
CHUNK_Z_MIN = -4
CHUNK_Z_MAX = 2
BLOCK_COUNT = CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE

WORLD_SEED = 0x4840BEEF
STONE_TRIES_PER_CHUNK = 0  # stays flat -- no extra stone blobs spawned

FLOOR_Y = 4
WORK_Y = 5  # circuit y-level (top of the planks floor)


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


REPEATER_BLOCKS = {
    "BLOCK_REPEATER_OFF",
    "BLOCK_REPEATER_ON",
    "BLOCK_REPEATER_EAST_OFF",
    "BLOCK_REPEATER_SOUTH_OFF",
    "BLOCK_REPEATER_WEST_OFF",
    "BLOCK_REPEATER_EAST_ON",
    "BLOCK_REPEATER_SOUTH_ON",
    "BLOCK_REPEATER_WEST_ON",
}


def floor_div(value, divisor):
    return value // divisor


def block_index(x, y, z):
    return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x


class LabWorld:
    def __init__(self):
        self.blocks = {}
        self.redstone = {}
        for cx in range(CHUNK_X_MIN, CHUNK_X_MAX + 1):
            for cz in range(CHUNK_Z_MIN, CHUNK_Z_MAX + 1):
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
                            elif y == FLOOR_Y:
                                block_data[idx] = B["BLOCK_PLANKS"]
                self.blocks[(cx, cz)] = block_data
                self.redstone[(cx, cz)] = redstone_data

    def set(self, wx, wy, wz, name, delay=1):
        if wy < 0 or wy >= CHUNK_HEIGHT:
            raise ValueError(f"y out of range: {wy}")
        cx = floor_div(wx, CHUNK_SIZE)
        cz = floor_div(wz, CHUNK_SIZE)
        if (cx, cz) not in self.blocks:
            raise ValueError(f"coord outside saved chunks: {(wx, wy, wz)}")
        lx = wx - cx * CHUNK_SIZE
        lz = wz - cz * CHUNK_SIZE
        idx = block_index(lx, wy, lz)
        self.blocks[(cx, cz)][idx] = B[name]
        self.redstone[(cx, cz)][idx] = delay if name in REPEATER_BLOCKS else 0

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
                header
                + bytes(block_data)
                + bytes(BLOCK_COUNT)
                + bytes(self.redstone[(cx, cz)])
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


# ----------------------------------------------------------------------------
# Wire helpers.
# ----------------------------------------------------------------------------
def wire_x(world, x0, x1, y, z):
    step = 1 if x0 <= x1 else -1
    for x in range(x0, x1 + step, step):
        world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")


def wire_z(world, x, y, z0, z1):
    step = 1 if z0 <= z1 else -1
    for z in range(z0, z1 + step, step):
        world.set(x, y, z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")


# ----------------------------------------------------------------------------
# Demo 1 -- memory cell (SR latch via cross-coupled torches).
# ----------------------------------------------------------------------------
# Layout (top-down at y=5).  S = stone, T = torch, w = wire, B = button.
#
#   z=mem_z-1               .   .  Q-LAMP (lit when Q=1)
#   z=mem_z      S1  T1  w  w  w  w  w  w  w  w  w  T2  S2
#                ^ B(reset on top of S1)         ^ B(set on top of S2)
#   z=mem_z+1                w   .  !Q-LAMP (lit when Q=0)
#
# T1 outputs FRONT (-z) onto z=mem_z-1, the Q chain.  T2 outputs BACK (+z)
# onto z=mem_z+1, the !Q chain.  Each chain decays from 15 down to ~4 at the
# far end, where it weakly powers the *opposite* stone -- that's the cross
# coupling.  Pressing the SET button (on S2) drives S2 high directly,
# flipping T2 OFF and latching the cell to Q=1.  RESET is the symmetric
# operation on S1.
def place_memory_cell(world, base_x, base_z):
    y = WORK_Y
    z_main = base_z
    z_q = base_z - 1
    z_nq = base_z + 1
    s1_x = base_x
    s2_x = base_x + 11
    t1_x = s1_x + 1
    t2_x = s2_x - 1

    # Stones + torches.  Initialise asymmetrically (T1 OFF, T2 ON) so the
    # latch settles into the well-defined Q=0 state on first tick instead of
    # oscillating in the symmetric metastable case.
    world.set(s1_x, y, z_main, "BLOCK_STONE")
    world.set(s2_x, y, z_main, "BLOCK_STONE")
    world.set(t1_x, y, z_main, "BLOCK_REDSTONE_TORCH_OFF")
    world.set(t2_x, y, z_main, "BLOCK_REDSTONE_TORCH_ON")

    # Q chain (T1 FRONT) and !Q chain (T2 BACK).
    wire_x(world, t1_x, t2_x, y, z_q)
    wire_x(world, t1_x, t2_x, y, z_nq)

    # Buttons sit on top of the stones; pressing pulses 15 directly into the
    # stone via the button's downward neighbour.
    world.set(s1_x, y + 1, z_main, "BLOCK_BUTTON")  # RESET (Q -> 0)
    world.set(s2_x, y + 1, z_main, "BLOCK_BUTTON")  # SET   (Q -> 1)

    # Indicator lamps tap the middle of each chain.
    mid_x = (t1_x + t2_x) // 2
    world.set(mid_x, y, z_q - 1, "BLOCK_LAMP_OFF")     # Q=1 lamp
    world.set(mid_x, y, z_nq + 1, "BLOCK_LAMP_OFF")    # Q=0 lamp


# ----------------------------------------------------------------------------
# Demo 2 -- comparator (side input vetoes a decayed rear input).
# ----------------------------------------------------------------------------
# Layout (top-down at y=5):
#
#   z=base_z      L  w  w  w  w  w  w  w  w  C  w  w  Lamp
#   z=base_z+1                                S
#
# L = lever, w = redstone wire, C = east-facing comparator, S = side lever.
# The rear lever's signal walks 9 wires before reaching the comparator's
# rear face, decaying to strength 7.  When the south-side lever is OFF the
# comparator outputs 15 (rear=7 >= side=0), lighting the lamp.  Toggling
# the side lever ON gives side=15 which exceeds rear=7, so the comparator
# turns off and the lamp goes dark.
def place_comparator_demo(world, base_x, base_z):
    y = WORK_Y
    rear_lever_x = base_x
    comp_x = base_x + 10

    world.set(rear_lever_x, y, base_z, "BLOCK_LEVER_ON")
    wire_x(world, rear_lever_x + 1, comp_x - 1, y, base_z)

    world.set(comp_x, y, base_z, "BLOCK_COMPARATOR_EAST_OFF")
    world.set(comp_x, y, base_z + 1, "BLOCK_LEVER_OFF")  # side input

    wire_x(world, comp_x + 1, comp_x + 2, y, base_z)
    world.set(comp_x + 3, y, base_z, "BLOCK_LAMP_OFF")


# ----------------------------------------------------------------------------
# Demo 3 -- redstone clock gated by a button.
# ----------------------------------------------------------------------------
# Layout (top-down at y=5):
#
#   z=base_z-1                                      Lamp (lit on each tick)
#   z=base_z   Sg Tg w  w  w  w  w  w  Sc Ct w  w  w  R  w
#              ^ B (button on top of Sg)
#   z=base_z+1                                            w
#   z=base_z+2 w  w  w  w  w  w  w  w  w  w  w  w  w  w
#
# Top row from left to right:
#   Sg, Tg : NOT cell that drives the gate output east toward the clock.
#   gate-output wires (7 of them): carry the gate output.  When the button is
#       NOT pressed, Sg is unpowered -> Tg is ON -> chain HIGH -> the
#       chain's last wire (corner cell) weakly powers Sc -> the clock
#       torch Ct stays OFF -> clock is dead.  Pressing the button powers
#       Sg, Tg flips OFF, the gate chain goes LOW, and the corner is then
#       only powered by the loop return wire that comes around from the
#       east -- so Ct is free to oscillate.
#   Sc, Ct : the clock torch and its support stone.
#   loop east of Ct: 3 wires + a delay-2 east-facing repeater + 1 wire.
# Bottom rows are the loop's return path: south, west, north back into the
# corner cell west of Sc.  The repeater gives the loop a real time delay so
# the torch oscillates every ~2 repeater ticks (~0.4 s).
def place_redstone_clock(world, base_x, base_z):
    y = WORK_Y

    # ---- Gate (NOT cell driven by a button on top of Sg) -----------------
    sg_x = base_x
    tg_x = sg_x + 1
    world.set(sg_x, y, base_z, "BLOCK_STONE")
    world.set(tg_x, y, base_z, "BLOCK_REDSTONE_TORCH_ON")
    world.set(sg_x, y + 1, base_z, "BLOCK_BUTTON")

    # ---- Clock loop ------------------------------------------------------
    sc_x = base_x + 8       # clock bias stone
    ct_x = sc_x + 1         # clock torch (Ct)
    rep_x = ct_x + 4        # repeater
    far_x = rep_x + 1       # last wire after the repeater

    world.set(sc_x, y, base_z, "BLOCK_STONE")
    world.set(ct_x, y, base_z, "BLOCK_REDSTONE_TORCH_ON")

    # Gate-output chain runs east from Tg into the wire immediately west of
    # Sc.  That wire is shared with the loop return -- it's the corner cell.
    wire_x(world, tg_x + 1, sc_x - 1, y, base_z)

    # Loop main row (east of Ct): wires + repeater + 1 wire.
    wire_x(world, ct_x + 1, rep_x - 1, y, base_z)
    world.set(rep_x, y, base_z, "BLOCK_REPEATER_EAST_OFF", delay=2)
    world.set(far_x, y, base_z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")

    # Loop return: south column at far east, then west across z=base_z+2,
    # then north column back into the corner west of Sc.
    wire_z(world, far_x, y, base_z + 1, base_z + 2)
    wire_x(world, sc_x - 1, far_x - 1, y, base_z + 2)
    world.set(sc_x - 1, y, base_z + 1, "BLOCK_REDSTONE_WIRE_UNCONNECTED")

    # Indicator lamp adjacent to a loop wire so the user can SEE the
    # oscillation.
    world.set(ct_x + 2, y, base_z - 1, "BLOCK_LAMP_OFF")


# ----------------------------------------------------------------------------
# Demo 4 -- 1-bit SR-latched 7-segment display.
# ----------------------------------------------------------------------------
# A real binary-counting T flip-flop in this redstone simulator takes ~25
# blocks per bit (see place_t_flip_flop in the original redstone_lab) -- far
# too large to fit in 3 chunks alongside the other demos.  This demo instead
# wires up a one-bit SR latch driving the segments, so:
#
#     RESET  -> Q=0 -> display "0" (segments A,B,C,D,E,F all lit, G dark)
#     SET    -> Q=1 -> display "1" (only segments B and C lit)
#
# B and C are constantly lit by two redstone blocks placed east of them.
# The other four segments (A, D, E, F) are lit only when !Q is HIGH (i.e.,
# when the latch is in the Q=0 state) so the result is a 7-segment "0" or
# "1".
#
# Pipeline:
#   * SR latch (cross-coupled torches) at z=lat_z, x=lat_x_left..lat_x_right.
#   * !Q wire at z=lat_z+1 ends adjacent to the buffer's input stone.
#   * 2-stage NOT-NOT buffer at z=lat_z+2..lat_z+3 regenerates !Q to a
#     full-strength signal on the bus origin at x=2, z=lat_z+1.
#   * North-bound bus at x=2 with one NORTH-facing repeater to refresh
#     mid-way.
#   * Branch wires fan out east at z=disp_z-1, +1, +3, +5 to drive the
#     A/F/E/D lamp drivers respectively.
#   * Two redstone blocks east of B and C provide the always-on power.
def place_seven_segment_counter(world, disp_x, disp_z, lat_z):
    y = WORK_Y

    # ---------- 7-segment lamp positions (fixed relative to disp origin) --
    # Display laid out flat on the floor, viewed from above:
    #
    #   . A .       z = disp_z+0
    #   F . B       z = disp_z+1
    #   . G .       z = disp_z+2
    #   E . C       z = disp_z+3
    #   . D .       z = disp_z+4
    seg = {
        "A": (disp_x + 1, disp_z + 0),
        "F": (disp_x + 0, disp_z + 1),
        "B": (disp_x + 2, disp_z + 1),
        "G": (disp_x + 1, disp_z + 2),
        "E": (disp_x + 0, disp_z + 3),
        "C": (disp_x + 2, disp_z + 3),
        "D": (disp_x + 1, disp_z + 4),
    }
    for name, (lx, lz) in seg.items():
        world.set(lx, y, lz, "BLOCK_LAMP_OFF")

    # Always-on segments (B and C) get a redstone block on their east side.
    world.set(disp_x + 3, y, disp_z + 1, "BLOCK_REDSTONE_BLOCK")
    world.set(disp_x + 3, y, disp_z + 3, "BLOCK_REDSTONE_BLOCK")

    # ---------- SR latch (same primitive as the memory cell) --------------
    s1_x = disp_x          # left stone
    s2_x = disp_x + 9      # right stone
    t1_x = s1_x + 1
    t2_x = s2_x - 1

    world.set(s1_x, y, lat_z, "BLOCK_STONE")
    world.set(s2_x, y, lat_z, "BLOCK_STONE")
    world.set(t1_x, y, lat_z, "BLOCK_REDSTONE_TORCH_OFF")  # Q=0 initial
    world.set(t2_x, y, lat_z, "BLOCK_REDSTONE_TORCH_ON")
    wire_x(world, t1_x, t2_x, y, lat_z - 1)  # Q chain (FRONT of T1)
    wire_x(world, t1_x, t2_x, y, lat_z + 1)  # !Q chain (BACK of T2)

    world.set(s1_x, y + 1, lat_z, "BLOCK_BUTTON")  # RESET button
    world.set(s2_x, y + 1, lat_z, "BLOCK_BUTTON")  # SET button (= "increment")

    # ---------- !Q -> bus buffer (double-NOT for signal regeneration) -----
    # Tap the !Q chain at its west end (s1_x, y, lat_z+1).  Build a small
    # NOT-NOT chain that climbs out of the way (south of the latch) and
    # re-emerges at the bus origin at x=2, y, lat_z+1.
    sa_x, sa_z = s1_x, lat_z + 2          # stone A: weakly powered by !Q
    ta_x, ta_z = s1_x, lat_z + 3          # torch A: NOT(!Q) = Q
    bridge_x, bridge_z = s1_x - 1, ta_z   # wire carrying Q toward stone B
    sb_x, sb_z = s1_x - 2, ta_z           # stone B: weakly powered by Q
    tb_x, tb_z = s1_x - 2, ta_z - 1       # torch B: NOT(Q) = !Q
    bus_origin_x = sb_x                   # = s1_x - 2 = 2 when s1_x = 4

    world.set(sa_x, y, sa_z, "BLOCK_STONE")
    world.set(ta_x, y, ta_z, "BLOCK_REDSTONE_TORCH_OFF")  # !Q HIGH -> Ta OFF
    world.set(bridge_x, y, bridge_z, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    world.set(sb_x, y, sb_z, "BLOCK_STONE")
    world.set(tb_x, y, tb_z, "BLOCK_REDSTONE_TORCH_ON")   # Q LOW -> Tb ON

    # ---------- North-bound bus (x=bus_origin_x) --------------------------
    bus_x = bus_origin_x
    bus_z_north = disp_z - 1   # one cell north of A row, drives A
    bus_z_south = lat_z + 1    # bus origin (Tb's FRONT output cell)
    rep_z = disp_z + 6         # repeater between display and latch

    # Bus wires from origin north to bus_z_north, with a repeater at rep_z.
    wire_z(world, bus_x, y, rep_z + 1, bus_z_south)
    world.set(bus_x, y, rep_z, "BLOCK_REPEATER_ON", delay=1)
    wire_z(world, bus_x, y, bus_z_north, rep_z - 1)

    # ---------- Branches that drive the four !Q-controlled segments ------
    # Branch z=disp_z-1: drives A (3 wires east to (disp_x+1, y, disp_z-1)).
    wire_x(world, bus_x + 1, disp_x + 1, y, disp_z - 1)
    # Branch z=disp_z+1: drives F at (disp_x, y, disp_z+1).  Single wire.
    world.set(bus_x + 1, y, disp_z + 1, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    # Branch z=disp_z+3: drives E at (disp_x, y, disp_z+3).  Single wire.
    world.set(bus_x + 1, y, disp_z + 3, "BLOCK_REDSTONE_WIRE_UNCONNECTED")
    # Branch z=disp_z+5: drives D (3 wires east to (disp_x+1, y, disp_z+5)).
    wire_x(world, bus_x + 1, disp_x + 1, y, disp_z + 5)


# ----------------------------------------------------------------------------
def main():
    world = LabWorld()
    place_memory_cell(world, base_x=-15, base_z=6)
    place_comparator_demo(world, base_x=4, base_z=6)
    place_redstone_clock(world, base_x=-12, base_z=-15)
    place_seven_segment_counter(world, disp_x=4, disp_z=0, lat_z=10)
    world.write()


if __name__ == "__main__":
    main()
