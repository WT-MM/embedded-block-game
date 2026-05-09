Voxel GPU RTL Guide
===================

This directory contains the RTL for the custom voxel GPU. The important mental
model is: software sends packed quad descriptors, the GPU rasterizes them into
one 640x60 on-chip band cache at a time, dirty cache rows flush to SDRAM, and
VGA scanout reads the completed SDRAM front buffer.

Files
-----

  * `voxel_gpu.sv` - top-level Avalon-MM peripheral, descriptor FIFO, main
    engine FSM, pixel pipeline, SDRAM arbitration, and VGA scanout wiring.
  * `voxel_raster_helpers.svh` - pure helper functions for coordinate clamps,
    band math, sky-gradient indexing, reciprocal setup, and explicit signed
    extension.
  * `voxel_color_helpers.svh` - pure helper functions for RGB565 conversion,
    alpha blending, scanout channel expansion, and palette light-bank remapping.
  * `voxel_sdp_ram.sv` - small RAM wrappers used by the band caches.
  * `voxel_texture_rom.sv` - explicit texture ROM wrapper with fixed read
    latency.
  * `voxel_vga_counters.sv` - 640x480 VGA timing generator.

Main Data Flow
--------------

1. HPS writes control registers and descriptor words through the Avalon-MM
   slave interface.
2. `ST_FETCH` collects one descriptor into `desc_words`.
3. `ST_SETUP` converts the descriptor into edge functions, depth gradients, and
   perspective-correct UV gradients.
4. `ST_DRAW` walks the descriptor bounding box two pixels per cycle. Pixels flow
   through reciprocal, texture, palette, fog, and commit stages.
5. Color/Z commits update the resident band cache. Untouched color pixels are
   lazy-cleared by treating `Z_CLEAR_SENTINEL` as sky/clear color at flush time.
6. Dirty band caches flush to the inactive SDRAM frame. `FLIP` makes the
   completed SDRAM frame visible on vsync.

State Groups
------------

  * Descriptor/raster path: `ST_IDLE`, `ST_FETCH`, `ST_SETUP`, `ST_DRAW`,
    `ST_DRAW_FLUSH`.
  * Cache writeback path: `ST_CACHE_EVICT`, `ST_CACHE_FLUSH_COLOR`,
    `ST_CACHE_FLUSH_Z`.
  * Cache fill/init path: `ST_CACHE_SELECT_FILL`, `ST_CACHE_INIT`,
    `ST_CACHE_LOAD_COLOR`, `ST_CACHE_START_LOAD_Z`, `ST_CACHE_LOAD_Z`.
  * SDRAM read safety path: `ST_CACHE_DRAIN_COLOR`, `ST_CACHE_DRAIN_Z`.

Why Some Gates Look Defensive
-----------------------------

Most subtle bugs came from concurrency between three users:

  * the main raster/cache-maintenance FSM
  * the background SDRAM flush path
  * VGA scanout line fills

The RTL therefore has several gates that wait for FIFO drain, linebuffer
headroom, or cache-port ownership. Before simplifying one of those gates, check
the matching notes in `PROJECT_NOTES.md` and the archived debug ledger under
`docs/notes/`.

Presentation Anchors
--------------------

For a quick walkthrough, follow these landmarks in `voxel_gpu.sv`:

  * Header comment: high-level architecture and reading map.
  * `engine_state_t`: lifecycle of one descriptor and one band cache.
  * `desc_*` wires: how packed descriptor words become raster inputs.
  * `edge_next_pair*` / `edge_next_row*`: the two-pixel draw loop stepping.
  * `ST_DRAW`: pixel pipeline staging.
  * `ST_CACHE_*`: cache init/load/flush lifecycle.
  * bottom perf-counter block: how software measures draw/load/flush time.
