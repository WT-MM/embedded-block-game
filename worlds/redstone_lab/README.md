Redstone Lab
============

Tracked ultraflat sample world for redstone experiments.

Landmarks from spawn:

- North-west plot: comparator rear-input and side-input demonstrations.
- North plot: a torch clock loop with two repeaters.
- West plot: a comparator latch. The west button sets it, the north button
  resets it, and the lamp marks the latched output.
- South plot: a real three-bit ripple counter wired into a constructed
  seven-segment decoder matrix. The button just below the south-east digit
  advances the count, the side buttons reset the bits, the three nearby lamps
  expose Q0/Q1/Q2, and the large lamp digit shows the decoded value. To clear
  the counter manually, reset Q0, then Q1, then Q2.
- South-east display matrix: the Q/NQ literal buses, invalid-row torches,
  segment taps, repeaters, segment bars, and lamps are all normal redstone
  parts fed by the counter outputs.

The old crafting-table display hook has been removed. Buttons now only inject
normal redstone power; any state change in this world must come from the visible
dust, repeaters, comparators, torches, and powered blocks.

The ultraflat floor is planks, with the redstone placed directly on top.

Right-click a repeater in-game to cycle its saved delay from 1 to 4 redstone
ticks.

Regenerate the raw tracked world with
`python3 worlds/redstone_lab/generate_redstone_lab.py` from the repository root,
then load it once and reset the three counter bits to save a settled zero state.
