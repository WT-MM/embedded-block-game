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

Reset is asynchronous per flip-flop. In this lab layout, clear from low bit to
high bit (`Q0`, then `Q1`, then `Q2`) before stepping the counter.

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
