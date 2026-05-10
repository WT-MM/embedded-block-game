Counter Circuit
===============

Target behavior: a visible, real redstone, three-bit modulo-8 ripple counter
with reset controls and a constructed seven-segment decoder.

Counter netlist
---------------

State bits are `Q0`, `Q1`, and `Q2`, where `Q0` is the least significant bit.
The count button must create one clean edge pulse.

The count input drives `Q0`. The `NQ0` output directly clocks `Q1`, and `NQ1`
directly clocks `Q2`, matching the tested ripple-counter fixture in
`sw/tests/world_chunk_test.c`.

Reset is asynchronous per flip-flop. The compact torch-cell version should use
one `CLR` rail that powers each reset support block directly and inhibits
count/carry pulse gates while reset is held.

Compact redesign prep
---------------------

The active lab still uses the tested comparator-latch fixture. The next layout
target is a short bus feeding local torch cells, not row-wide wires.

Button input should first enter a two-by-two rising-edge detector core:

```
[keepout ] [side repeater]
[rear tap] [comparator   ] -> pulse out
```

The clock bus owns the two adjacent feed cells: one direct rear tap into the
comparator, and one delayed feed into the side repeater. The rear tap must be
weaker than the delayed side input so the comparator emits only the initial
edge. The output pad is immediately to the comparator's front and is not part
of the two-by-two core.

Each counter bit should become a local torch-gate toggle cell with reset, and
the carry edge should be routed into the next bit with adjacent cells or
vertical torch transfer instead of long dust runs.

Proposed compact primitives
---------------------------

Symbols:

```
w  redstone dust          r> repeater facing east
c> comparator facing east #  solid support/powered block
t> torch mounted on west block, output to the east
<t torch mounted on east block, output to the west
P  pulse/input rail       Q  latch output
N  inverted latch output  CLR reset/inhibit rail
```

Side torch inverter, core two cells:

```
in -> w [#] [t>] w -> out
```

The torch must remember the support face for `#`. In generated chunks, prefer
writing the torch support metadata rather than relying on fallback support
inference.

Two-by-two rising-edge detector core:

```
          delayed feed
              |
        [ . ] [r v]
input ->[ w ] [c>] -> pulse
```

The direct rear tap should be one dust step weaker than the repeater output.
That makes the comparator emit the first edge and turn off once the delayed
side input arrives.

Cross-coupled torch SR latch, three-by-three core:

```
z0: [#R] [t>] [Q ]
z1: [N ] [ .] [Q ]
z2: [N ] [<t] [#S]
```

Power `#S` to set `Q=1`. Power `#R` to reset `Q=0`. The torch beside `#R`
drives the `Q` rail, and the torch beside `#S` drives the `N` rail.

Conditional pulse gate:

```
z0: P -> [#P] [tN] [w ]
z1:           .    [#G] [tG] -> out
z2:                blocker
```

`tN` is `not P`. `tG` outputs `P AND NOT blocker`. Keep the blocker rail on
the far side of `#G` so `tN` cannot backfeed it. A `CLR` wire can feed `#G` as
a second blocker so reset pulses do not become count pulses.

Toggle bit cell direction:

```
single SR latch + set/reset steering gates: not robust enough
master SR latch + slave SR latch: target compact bit primitive
```

The single-latch T sketch races in this engine: a one-update set pulse is too
short for the cross-coupled torch latch to settle, but holding it longer lets
the opposite steering gate see the changed state and fire. The next compact
counter should therefore use a master/slave torch bit:

```
master enable = not clock
master D      = not slave.Q
slave enable  = clock
slave D       = master.Q

Q  = slave.Q
N  = slave.N
CLR powers both reset supports and inhibits all steering gates
```

Ripple rule for an incrementing three-bit counter remains:

```
button level -> bit0 clock
bit0 N       -> bit1 clock
bit1 N       -> bit2 clock
```

The carry uses `N` because `N` rises when `Q` falls, so higher bits toggle on
the overflow edge of the lower bit.

Seven-segment decoder
---------------------

Segments are `A` top, `B` upper right, `C` lower right, `D` bottom,
`E` lower left, `F` upper left, and `G` middle.

```
digit  Q2 Q1 Q0  segments
0      0  0  0   A B C D E F
1      0  0  1   B C
2      0  1  0   A B D E G
3      0  1  1   A B C D G
4      1  0  0   B C F G
5      1  0  1   A C D F G
6      1  1  0   A C D E F G
7      1  1  1   A B C
```

Layout constraints
------------------

- Treat the layout like a two-layer board: named buses first, then gates,
  then display fanout.
- Keep the count control next to the visible display.
- The complete live counter/display path should fit inside the chunks loaded
  around that operator position at low render distance.
- Do not run a segment bus through another segment's driver row or lamp row.
- Segment lamps are driven directly by the constructed decoder outputs.
- Prefer short named nets over long row-wide wires. If a net must run farther
  than a few blocks, it should be an intentional bus with repeaters and no
  adjacent foreign net.
- A generator-side layout validator should reject adjacent cells belonging to
  different named nets unless the connection is an explicit gate, repeater, or
  powered block.
