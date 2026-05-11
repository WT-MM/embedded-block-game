# Final Technical Breakdown

## Project Overview

This is a Minecraft-style voxel game split across HPS-side C software, a Linux kernel device driver, Platform Designer system integration, and custom SystemVerilog RTL. The main system context is `soc_system` in `hw/soc_system.qsys`; the board wrapper is `hw/soc_system_top.sv`; the main custom FPGA datapath is `hw/voxel_gpu/rtl/voxel_gpu.sv`.

## Evidence From Source

- `hw/soc_system.qsys` instantiates `voxel_gpu_0` of kind `voxel_gpu`.
- `hps_0.h2f_lw_axi_master` connects to `voxel_gpu_0.avalon_slave_0` at base `0x0000`.
- `hps_0.h2f_axi_master` connects to `fpga_sdram.s1` at base `0x0000`.
- `hw/voxel_gpu_hw.tcl` declares the `voxel_gpu` Avalon slave, VGA conduit, and `voxel_sdram` conduit.
- `sw/voxel_gpu.c` exposes `/dev/voxel_gpu`, maps the FPGA register resource, handles ioctls, and streams FIFO words.
- `sw/renderer.c` builds `struct quad_desc` / `struct quad_desc_uv` descriptors and submits them through `sw/gpu_transport.c`.

## HPS/Software Architecture

`sw/game.c::main()` owns the frame loop. It updates input, player physics, environment simulation, redstone, falling blocks, chunk streaming, lighting, async chunk generation, and mesh draining. It then calls `renderer_begin_frame()`, draw functions for sky/world/entities/UI, and `renderer_end_frame()`.

`VoxelWorld` in `sw/world.h` owns chunk arrays, block IDs, lighting, fluid/redstone metadata, live `ChunkMesh` snapshots, streaming state, and worker synchronization. The renderer reads immutable `ChunkMesh` snapshots and emits packed descriptors into `RenderContext::submit_buffer`.

`sw/gpu_transport.c` owns hardware submission policy: per-band descriptor bins, optional band reuse, `BEGIN_BAND` / descriptor `write()` / `END_BAND`, and flip handling. The kernel driver does not parse descriptors.

## FPGA/Hardware Architecture

`voxel_gpu` contains the CSR/FIFO front door, descriptor fetch, raster setup, two-pixel draw walk, reciprocal/texture/palette/fog pipeline, color/Z commit, ping-pong band caches, SDRAM write/read arbitration, line buffers, and VGA output.

Major instantiated modules:

- `sdram_ctrl`: `Sdram_Control`
- `fb_back_ram_A`: `voxel_banked_sdp_ram`
- `fb_back_ram_B`: `voxel_banked_sdp_ram`
- `z_ram_A`: `voxel_banked_sdp_ram`
- `z_ram_B`: `voxel_banked_sdp_ram`
- `draw_step`: `voxel_draw_step`
- `fog_blend_lane0`: `voxel_fog_blend`
- `fog_blend_lane1`: `voxel_fog_blend`
- `iw_norm_lane0`: `voxel_iw_normalize`
- `iw_norm_lane1`: `voxel_iw_normalize`
- `raster_setup`: `voxel_raster_setup`
- `recip_interp_lane0`: `voxel_recip_interpolate`
- `recip_interp_lane1`: `voxel_recip_interpolate`
- `texture_rom`: `voxel_texture_rom`
- `counters`: `voxel_vga_counters`
- `w_denorm_lane0`: `voxel_w_denormalize`
- `w_denorm_lane1`: `voxel_w_denormalize`

## soc_system Overview

`soc_system_top.sv` instantiates `soc_system soc_system0` and exports HPS DDR3 pins, VGA pins, and the voxel SDRAM pins. The `.qsys` file shows `sdram_clocks.sys_clk` feeding the FPGA SDRAM controller, `voxel_gpu_0.clock`, and HPS bridge clocks. The generated `soc_system` HDL is not present in this repo snapshot, so system internals beyond `.qsys` are documented from the Platform Designer file rather than guessed.

## End-To-End Game-To-Pixels Dataflow

World/chunk state flows from `VoxelWorld` and `ChunkMesh` into `renderer_draw_world()`. `stage_prepared_quad()` computes screen-space bounds, edge coefficients, depth gradients, and optional perspective-correct UV planes. `gpu_transport_bin_descriptor()` clips descriptors into eight 60-line bands. The kernel driver writes CSR commands and descriptor words into the `voxel_gpu` Avalon window. `voxel_gpu` fetches descriptors, rasterizes pixels into a resident color/Z band cache, flushes dirty rows to the inactive SDRAM frame, and scanout reads the active SDRAM frame into VGA line buffers.

## HPS-To-FPGA Interface

The register map is in `docs/diagrams/register_map.md`. Control/status facts are extracted from `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, and `hw/voxel_gpu/rtl/voxel_gpu.sv`. The FIFO window is `0x1000..0x1FFF` in C byte offsets and `ADDR_FIFO_LO=13'h400` through `ADDR_FIFO_HI=13'h800` in RTL word addresses.

The driver synchronizes by polling `STATUS.FEM`, `STATUS.BSY`, and `STATUS.VSY`. No interrupt output is visible in the `voxel_gpu` module ports; `CONTROL[2]` is stored as `ctrl_ien`, but no source-visible interrupt path is connected.

## voxel_gpu Datapath and Pipeline

The engine FSM states are: `ST_IDLE`, `ST_CLEAR`, `ST_FETCH`, `ST_SETUP`, `ST_DRAW`, `ST_DRAW_FLUSH`, `ST_CACHE_EVICT`, `ST_CACHE_FLUSH_COLOR`, `ST_CACHE_FLUSH_Z`, `ST_CACHE_SELECT_FILL`, `ST_CACHE_INIT`, `ST_CACHE_LOAD_COLOR`, `ST_CACHE_START_LOAD_Z`, `ST_CACHE_LOAD_Z`, `ST_CACHE_DRAIN_COLOR`, `ST_CACHE_DRAIN_Z`.

The draw pipeline uses named register groups visible in RTL: `pipe0`, `recip0`, `recip1`, `recip2`, `pipe1`, `tex0`, `pipe2`, `draw_pipe`, `pal_rd`, `plr`, `fog0`, `fog1`, and `commit`, with duplicate `_o` groups for the odd lane. Operators visible in the source include edge/depth/UV adders, edge/bbox/z comparators, reciprocal normalization shifts, reciprocal interpolation multiply/subtract, UV multiply, texture coordinate selection, palette/light-bank selection, radial fog multiply/add, alpha/fog blending, and color/Z RAM writes.

Important memories and buffers are `fifo_mem`, `palette`, `sky_palette`, `recip_lut`, the dual-read `voxel_texture_rom`, four banked color/Z band RAMs, three scanout line buffers, and external board SDRAM frames.

## Timing and Latency Evidence

The RTL explicitly pipelines palette read and texture ROM latency with named stages, and `DRAW_FLUSH_CYCLES=14` drains valid stages between fetch and color commit. VCD produced at `build/diagrams/voxel_gpu.vcd`; WaveJSON/SVG timing assets are derived from captured simulator signals. The current VCD comes from `tb/voxel_gpu_tb.sv`; `tb/sim_stubs.sv` supplies simulation-only models for the Quartus PLL/RAM/FIFO primitives. No cycle-accurate timing is claimed beyond captured simulator signals. Quartus Fmax/slack/resource/utilization values require Quartus report after full build.

## Netlist Evidence

Not produced in this environment. See `build/diagrams/yosys_voxel_gpu.log` for the exact command/error. If Yosys is installed, run `scripts/gen_netlist_json.sh` or `make diagrams` to produce `build/diagrams/voxel_gpu.json`.

## Testing and Validation Evidence In Repo

Software tests exist under `sw/tests/`, including renderer, world, inventory, command parser, player physics, FPGA SDRAM offset/band tests. A Python virtual hardware monitor exists under `virtual_hw/`, including raster tests. No dedicated RTL testbench directory existed before this workflow; `tb/voxel_gpu_tb.sv` is a minimal simulation-only harness.

## Known Limitations and Current Behavior

- No Quartus timing/resource reports are available in this repo snapshot, so no Fmax, slack, ALM/LUT/DSP/RAM/register counts, or timing-closure claims are made.
- Generated Platform Designer HDL under `hw/soc_system/synthesis/` is not present, so `soc_system` diagrams use `.qsys` and wrapper evidence.
- `voxel_gpu_timing.wave.json` and `voxel_gpu_timing.svg` are VCD-derived when `build/diagrams/voxel_gpu.vcd` exists; otherwise the WaveDrom file is a marked skeleton.
- The VCD uses simulation-only primitive stubs and minimal MMIO/FIFO stimulus, so it is not a Quartus timing report or a full rendered-scene validation.
- The Yosys netlist flow depends on Yosys and may also need vendor primitive handling for `altsyncram`/FIFO/PLL components; the script emits an actionable log.
