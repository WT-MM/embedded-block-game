# File listings

A short tour of the repository, scoped to files that other documents
in `docs/` reference. The intent is to give a reviewer a map: "what
file should I open to verify claim X?". Counts and exact line numbers
are not pinned (they drift); the paths and the role of each file are.

## Hardware (FPGA)

### `hw/voxel_gpu/rtl/`

| File | Role |
|---|---|
| `voxel_gpu.sv` | Top-level `voxel_gpu` module. Avalon-MM register decode, FIFO_WINDOW, descriptor unpack, 16-state engine FSM, 13-stage pixel pipeline (lane0/lane1), band cache controllers, SDRAM flush state machines, palette / sky / recip LUT memories, ROM/RAM instantiations. |
| `voxel_raster_math.sv` | `voxel_raster_setup` (edge equations + Z/UV starts) and `voxel_draw_step` (lane1 + per-row/per-pair deltas, 64-bit multiplies). Pure combinational. |
| `voxel_recip_math.sv` | `voxel_iw_normalize`, `voxel_recip_interpolate`, `voxel_w_denormalize` — the three stages of the 1/w pipeline (per-lane). Pure combinational. |
| `voxel_fog_blend.sv` | Combinational fog + alpha blend. |
| `voxel_sdp_ram.sv` | `voxel_sdp_ram` (single SDP wrapping `altsyncram`) and `voxel_banked_sdp_ram` (even/odd x banking on `addr[0]`). |
| `voxel_texture_rom.sv` | Texture atlas ROM. Two `altsyncram` instances initialized from `voxel_gpu/assets/textures.mif`, port A = lane0, port B = lane1. |
| `voxel_vga_counters.sv` | 640×480 @ 25 MHz VGA timing from the 50 MHz fabric clock. |
| `voxel_raster_helpers.svh` | SV macros / functions used by `voxel_gpu.sv` (edge-test helpers, addressing math). |
| `voxel_color_helpers.svh` | RGB888 ↔ RGB565 helpers and palette light-bank application. |
| `README.md` | Higher-level RTL design notes. |

### `hw/voxel_gpu/assets/`

Texture atlas (`textures.mif`) and the reciprocal LUT (`recip_lut.hex`),
both consumed by Quartus via `altsyncram` `INIT_FILE` and the inferred
`recip_lut` array.

### `hw/sdram_local_test/`

Vendor SDR SDRAM controller and PLL, included as-is.

### `hw/`

| File | Role |
|---|---|
| `soc_system.qsys` | Platform Designer system (XML). Source of truth for the module graph and connections. `soc_system.v` is produced by `qsys-generate` and is not committed. |
| `soc_system.tcl` | Top-level project TCL. |
| `soc_system_top.sv` | Top-level wrapper that exports HPS pins + VGA + SDRAM pins. |
| `voxel_gpu_hw.tcl` | Platform Designer component manifest for the `voxel_gpu` component. Declares the Avalon slave attributes (WORDS, 32 b data, readWaitTime=1) and the `vga` / `voxel_sdram` conduits. |
| `Makefile` | Quartus / qsys / preloader / kernel / DTB orchestration. |

## Software (HPS)

### `sw/`

| File | Role |
|---|---|
| `game.c` | `main()`, main loop (input → world → render). Drives `renderer_begin_frame` / `renderer_end_frame`. |
| `world.c` / `world_gen.c` | World state and procedural generation. |
| `mesh_worker.c` / `gen_worker.c` | Background mesh + chunk generation threads. |
| `player_physics.c` | Player physics. |
| `input.c` | Keyboard + mouse input. |
| `inventory.c`, `game_items.c`, `game_home.c`, `pause_menu.c`, `chat.c`, `command_parser.c` | UI and game-state subsystems. |
| `renderer.c` | Quad-emission API (`renderer_draw_*`). Calls into `gpu_transport.c` only at frame boundaries. |
| `gpu_transport.c` | User-space transport: per-band descriptor bins, ioctl dispatch, performance-counter readback. Selects between hardware backend (`/dev/voxel_gpu`) and a socket-based virtual GPU (used by software-only tests). |
| `voxel_gpu.c` | Linux kernel platform driver. `voxel_open` / `voxel_release` / `voxel_write` (`iowrite32_rep` into FIFO_WINDOW) / `voxel_ioctl` / `voxel_poll_status`. |
| `voxel_gpu.h` | Shared user/kernel header. Register byte offsets, bit fields, ioctl numbers, packed descriptor structs. The single source of truth for the HPS-side register interface. |
| `gpu_transport.h`, `renderer.h`, `virtual_gpu_protocol.h` | Public APIs and the virtual-GPU socket protocol. |
| `block_types.{c,h}`, `texture_tiles.def` | Block / tile metadata, kept in sync with `textures.mif`. |
| `thread_affinity.c` | Pins worker threads to specific cores on the Cortex-A9. |
| `Makefile` | Builds the user-space binary and kernel module. |
| `tests/` | Unit tests for renderer, inventory, command parser, player physics, world chunking, fpga band offsets, sdram smoke. |

## Testbench

| File | Role |
|---|---|
| `tb/voxel_gpu_tb.sv` | Self-contained smoke testbench for `voxel_gpu`. Drives clock + reset, issues a handful of Avalon writes, tri-states SDRAM, dumps a VCD. Not a correctness test. |

## Diagram + breakdown tooling

| File | Role |
|---|---|
| `scripts/gen_netlist_json.sh` | Tries Yosys against the SV RTL with `altsyncram` blackboxed; falls back to `gen_pipeline_diagram.py --emit-structural-json` if Yosys is unavailable or fails. Always emits `build/diagrams/voxel_gpu.json` and `build/diagrams/soc_system.json`. |
| `scripts/gen_pipeline_diagram.py` | Generates `voxel_gpu_pipeline.mmd`, `voxel_gpu_module_hierarchy.mmd`, `voxel_gpu_control_fsm.mmd`. Can also emit the structural JSON used by the netlist script. |
| `scripts/gen_datapath_diagram.py` | Generates `voxel_gpu_datapath.mmd` (lane0/lane1 detailed datapath). |
| `scripts/gen_timing_diagram.py` | Generates the `voxel_gpu_timing_*.wave.json` family (WaveDrom): pipeline cadence, Avalon-MM write, band command sequence, cache flush, scanout linebuffer load, SDRAM-pin write/read, VGA pixel clock. |

## Docs

| File | Role |
|---|---|
| `docs/final_breakdown.md` | This-level technical overview. |
| `docs/file_listings.md` | This file. |
| `docs/diagrams/*.mmd`, `*.wave.json`, `*.md` | All generated and static diagrams + their breakdown markdown. See `docs/diagrams/diagram_index.md`. |
| `docs/notes/` | Long-form design and debugging notes that pre-date this breakdown pass. |
| `PROJECT_NOTES.md` | Historical project notes at the repo root. |
| `README.md` | Repo entry point. |
