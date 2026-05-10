Redstone Lab
============

Tracked ultraflat sample world for redstone experiments.

Landmarks from spawn:

- North-west plot: comparator rear-input and side-input demonstrations.
- North plot: a torch clock loop with two repeaters, plus a button-driven
  two-repeater pulse line into a lamp.
- South-west plot: a standalone T flip-flop. The west button is the clock
  input, the nearby side button resets the stored bit, and the lamp marks Q.
- South plot: a real three-bit ripple button counter made from the same T
  flip-flop cells. The west button advances the count, the side buttons reset
  the bits, and the three lamps expose Q0/Q1/Q2. To clear the counter manually,
  reset Q0, then Q1, then Q2.
- South-east plot: a constructed seven-segment decoder matrix. The literal
  buses, invalid-row torches, segment taps, repeaters, and lamps are all normal
  redstone parts; the saved source blocks currently drive the zero pattern.

The old crafting-table display hook has been removed. Buttons now only inject
normal redstone power; any state change in this world must come from the visible
dust, repeaters, comparators, torches, and powered blocks.

Right-click a repeater in-game to cycle its saved delay from 1 to 4 redstone
ticks.

Regenerate this tracked world with `python3 worlds/redstone_lab/generate_redstone_lab.py`
from the repository root.
