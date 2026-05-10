Redstone Lab
============

Tracked ultraflat sample world for redstone experiments.

Landmarks from spawn:

- North-west plot: comparator rear-input and side-input demonstrations.
- North plot: a torch clock loop with two repeaters.
- West plot: a comparator latch. The west button sets it, the north button
  resets it, and the lamp marks the latched output.
- South plot: a real three-bit ripple counter wired into a constructed
  seven-segment decoder matrix. The count button at roughly `-29 / 5 / 21`
  sits beside a ground-laid seven-segment display with three lamps per segment.
  The three reset buttons are beside the flip-flop cells; press them from low
  bit to high bit to clear the counter.
- South-east decoder matrix: the Q/NQ literal buses, invalid-row torches,
  segment taps, repeaters, and display feed columns are all normal redstone
  parts fed by the counter outputs.

The old crafting-table display hook has been removed. Buttons now only inject
normal redstone power; any state change in this world must come from the visible
dust, repeaters, comparators, torches, and powered blocks.

The ultraflat floor uses different materials to separate demos: stone for the
comparator area, logs for the clock, cobblestone for the latch, planks for the
flip-flops, and sandstone for the decoder/display area.

Right-click a repeater in-game to cycle its saved delay from 1 to 4 redstone
ticks.

Regenerate the raw tracked world with
`python3 worlds/redstone_lab/generate_redstone_lab.py` from the repository root.
The checked-in chunks are raw circuit chunks. Use the reset buttons to clear
the counter before stepping it.
