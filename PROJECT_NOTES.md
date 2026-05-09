Project Notes
=============

Current Status
--------------

The project is in the post-640x480 SDRAM renderer cleanup phase. The important
working assumptions are:

  * Preserve the SDRAM-backed eight-band render path and its ordering fixes.
  * Preserve dirty-rectangle/full-band reuse behavior unless a change is tested
    against cached/static frames and active camera motion.
  * Keep near chunks unmerged by default; greedy chunk meshing remains opt-in
    with `BLOCK_GAME_GREEDY_MESH=1`.
  * Keep async mesh rebuilds and async chunk generation as the default runtime
    shape, with the pause menu exposing stream/render-distance tuning.
  * Treat `VOXEL_TARGET_FPS`, `VOXEL_PIPELINE_FRAMES`,
    `VOXEL_SKY_PALETTE_STEP_SECONDS`, `VOXEL_OCCLUSION_CULL`, and stream/chunk
    env flags as supported debugging knobs.

Current Code Layout
-------------------

The larger C files are being split along ownership boundaries:

  * `sw/game.c` owns the main loop, camera, gameplay input, HUD, and inventory
    screen interaction.
  * `sw/game_home.c` owns world selection, save discovery, and home-menu drawing.
  * `sw/game_items.c` owns dropped-item entities, pickup, and inventory-close
    spill/drop behavior.
  * `sw/world.c` owns chunk storage, persistence, streaming, lighting, meshing,
    fluid/gravity simulation, and block mutation.
  * `sw/world_gen.c` owns deterministic biome/heightmap terrain generation.
  * `hw/voxel_gpu/rtl/voxel_gpu.sv` remains the main RTL module. Stateless
    arithmetic is split into `voxel_raster_math.sv`, `voxel_recip_math.sv`, and
    `voxel_fog_blend.sv`; shared helper functions live in
    `voxel_raster_helpers.svh` and `voxel_color_helpers.svh`. Keep FSM timing
    and ownership gates in the main file unless an extraction is proven by lint
    and hardware-targeted smoke tests. `hw/voxel_gpu/rtl/README.md` is the
    teammate-facing map for presentations and code walkthroughs.

Active Renderer Notes
---------------------

The current hardware path renders one 640x480 frame as eight 60-line bands.
Userspace bins descriptors per band, the RTL draws into ping-pong local caches,
background flush writes bands to SDRAM, and VGA scanout reads the visible SDRAM
frame through line buffers. Software culls opaque world faces with a
conservative 4x4 screen-tile occlusion grid before descriptor emission; it is
enabled by default and can be disabled with `VOXEL_OCCLUSION_CULL=0`.

The key bug classes already fixed were:

  * bottom-band corruption from cache-port ownership during final-band flushes
  * left-edge/column-0 artifacts from stale SDRAM read-FIFO tail words
  * visible-phase horizontal shift from scanout linebuffer handoff timing
  * sky/ground fast-frame flashing from `CLEAR_FRAME` and queued flush/begin
    ordering interactions
  * frame-rate stalls from small driver writes, serialized band flushes,
    redundant software descriptor binning, and cache init cost

Keep the archive handy before changing any of this. Several symptoms looked
similar on screen but had different root causes.

Validation Checklist
--------------------

Before calling a cleanup safe, run the narrow tests first, then the wider pass:

  * `make -C sw tests`
  * `sw/tests/voxel_test`
  * `sw/tests/command_parser_test`
  * `sw/tests/inventory_test`
  * `sw/tests/world_chunk_test`
  * `VOXEL_GPU_BACKEND=socket` renderer tests, as applicable
  * virtual hardware pytest for SDRAM protocol regressions
  * Verilator lint for `hw/voxel_gpu/rtl/voxel_gpu.sv`

Known acceptable RTL lint output is limited to boxed Intel megafunctions or
missing external module warnings from the local lint harness.

Archive
-------

The detailed bring-up/debug ledger was moved out of this top-level file so the
active notes stay readable:

  * `docs/notes/2026-debug-and-performance-history.md`

That archive preserves the full pre-cleanup `PROJECT_NOTES.md`, including SDRAM
bring-up notes, rasterizer/palette bugs, frame-rate work, async streaming, and
worldgen history.
