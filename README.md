Embedded Block Game
===================

Quick Start: Virtual Voxel GPU
------------------------------

Run the Python virtual GPU in one shell:

    cd virtual_hw
    uv sync
    uv run virtualhw --scale 4

Run the game against it in another shell:

    cd sw
    make game
    VOXEL_GPU_BACKEND=socket ./game

On macOS, build the desktop-safe test targets instead; the interactive game
uses Linux `/dev/input/event*` headers and devices:

    cd sw
    make tests
    VOXEL_GPU_BACKEND=socket ./tests/renderer_static_test

Useful Variants
---------------

Mirror commands to both the FPGA driver and the virtual GPU:

    cd sw
    VOXEL_GPU_BACKEND=tee ./game

Run the virtual GPU headless and dump frames:

    cd virtual_hw
    uv run virtualhw --headless --dump-dir /tmp/voxel_frames

Chat Commands
-------------

Press `T` in-game to open chat, or press `/` to open chat already seeded with
a slash. Lines starting with `/` are parsed as commands:

- Press `Tab` while typing a command to cycle valid completions for the
  current word, e.g. `/physics set <Tab>` cycles physics properties.
- Press `Up` while chat is open to recall previously submitted text or
  commands; press `Down` to move back toward the current draft.

- `/time set day` or `/time day`
- `/time set night` or `/time night`
- `/gamemode set survival` or `/gamemode survival`
- `/gamemode set creative` or `/gamemode creative`
- `/gamemode set spectator` or `/gamemode spectator`
- `/gm ...` and `/mode ...` are shorthand aliases for `/gamemode ...`.
- `/physics set gravity 12.5`, `/physics set speed 6`,
  `/physics set jump_height 2`, `/physics set fly_speed 10`, and
  `/physics reset`.
- `/setblock <x> <y> <z> <block>` edits one block.
- `/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>` edits a region.
- `/give <item> [count]` or `/give me <item> [count]` adds items to the
  player's survival inventory.
- `/items [page]` lists names accepted by `/give`.
- Coordinates can be absolute integers or Minecraft-style relative values
  such as `~`, `~-1`, and `~3`; block names use underscores, e.g.
  `air`, `stone`, `glass`, `diamond_block`.
- `/kill` respawns the player at spawn, preserving the current mode.
- `/help` prints the supported command forms in chat.

Notes
-----

- `VOXEL_GPU_BACKEND=hw` is the default and uses `/dev/voxel_gpu`.
- `VOXEL_GPU_BACKEND=socket` avoids the kernel driver entirely.
- `./game` reads keyboard and mouse input from `/dev/input/event*` on Linux.
- The input layer prefers relative mice, grabs pointer devices by default, and falls back to absolute VM tablet devices only when needed.
- Press `Esc` to pause and release the grabbed mouse; resuming gameplay re-captures it.
- The pause menu can adjust render distance, mouse sensitivity, and FOV at runtime.
- Use `VOXEL_MOUSE_INVERT_X=1` and/or `VOXEL_MOUSE_INVERT_Y=1` to flip axes.
- Use `VOXEL_MOUSE_SENS=0.004` to override mouse sensitivity at launch.
- Use `VOXEL_MOUSE_GRAB=0` to leave the guest cursor free, or `VOXEL_MOUSE_ALLOW_ABS=0` to disable absolute tablet fallback when a relative mouse is present.
- World edits are saved under `../worlds/default` when the game is launched from `sw/`; set `VOXEL_WORLD_DIR=/path/to/world` to use a different save root.
- The save format keeps a small `world.meta` file plus per-chunk snapshots in `chunks/<x>_<z>.chk`, layered on top of the procedural world seed.

If input only works under `sudo`, add your user to the `input` group and start
a new login session:

    sudo usermod -aG input $USER

More Detail
-----------

- `sw/README` covers the kernel module, renderer tests, and transport modes.
- `hw/README.md` covers Quartus / Platform Designer notes, including the
  Quartus 19.1 HDL-regeneration workaround for the SDRAM controller IP.
- `virtual_hw/README.md` covers the Python virtual GPU package and options.
- `PROJECT_NOTES.md` is the active engineering note index; long historical
  debug ledgers live under `docs/notes/`.
