Redstone Lab
============

Tracked ultraflat sample world for redstone experiments.

Landmarks from spawn:

- North-west plot: comparator rear-input and side-input demonstrations.
- North plot: a torch clock loop with two repeaters, plus a button-driven
  two-repeater pulse line into a lamp.
- South-west plot: a standalone T flip-flop. The west button is the clock
  input, the nearby side button resets the stored bit, and the lamp marks Q.
- South plot: a real two-bit ripple button counter made from the same T
  flip-flop cells. The west button advances the count, the side buttons reset
  the bits, and the two lamps expose Q0/Q1. To clear both bits manually, reset
  Q0 first and Q1 second.

The old crafting-table display hook has been removed. Buttons now only inject
normal redstone power; any state change in this world must come from the visible
dust, repeaters, comparators, torches, and powered blocks.

Right-click a repeater in-game to cycle its saved delay from 1 to 4 redstone
ticks.
