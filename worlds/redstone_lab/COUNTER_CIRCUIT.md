Counter Circuit
===============

Target behavior: a visible, real redstone, three-bit modulo-8 counter with
asynchronous reset and a constructed seven-segment decoder.

Counter netlist
---------------

State bits are `Q0`, `Q1`, and `Q2`, where `Q0` is the least significant bit.
The count button must create one clean edge pulse.

Equivalent next-state equations:

- `D0 = not Q0`
- `D1 = Q1 xor Q0`
- `D2 = Q2 xor (Q0 and Q1)`

Equivalent T flip-flop inputs:

- `T0 = 1`
- `T1 = Q0`
- `T2 = Q0 and Q1`

Reset forces all state bits low and must not pass through the count ripple
path, or later reset pulses can re-toggle an earlier-cleared bit.

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
- Keep the operator controls at the electrical center of the counter/display,
  not at one edge.
- The complete live counter/display path should fit inside the chunks loaded
  around that operator position at low render distance.
- Do not run a segment bus through another segment's driver row or lamp row.
- Horizontal display bars are inset between the side columns; top and bottom
  do not share the side-column lamp blocks.
- Prefer short named nets over long row-wide wires. If a net must run farther
  than a few blocks, it should be an intentional bus with repeaters and no
  adjacent foreign net.
- A generator-side layout validator should reject adjacent cells belonging to
  different named nets unless the connection is an explicit gate, repeater, or
  powered block.
