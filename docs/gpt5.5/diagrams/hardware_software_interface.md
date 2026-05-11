# Hardware/Software Interface

The HPS software talks to the FPGA through the kernel misc device `/dev/voxel_gpu` implemented in `sw/voxel_gpu.c`. The driver maps the Platform Designer resource with `of_iomap()` and exposes two access styles:

- `ioctl()` for control/status/palette/fog/external-memory/band/performance registers.
- `write()` for descriptor FIFO payloads, written with `iowrite32_rep()` to `VOXEL_FIFO_BASE`.

## Source-Grounded Path

`sw/game.c::main()` calls `renderer_begin_frame()`, rendering functions, and `renderer_end_frame()`. `sw/renderer.c::stage_prepared_quad()` builds packed `struct quad_desc` records and optional `struct quad_desc_uv` records defined in `sw/voxel_gpu.h`. `sw/gpu_transport.c` bins those descriptors into eight `VOXEL_BAND_CACHE_HEIGHT=60` line bands, then submits each band with:

1. `VOXEL_IOC_BEGIN_BAND`, which writes `BAND_INDEX`, `BAND_WINDOW`, and `BAND_CTRL.BEGIN`.
2. `write()`, which streams packed 32-bit descriptor words into the FIFO window.
3. `VOXEL_IOC_END_BAND`, which writes `BAND_CTRL.FLUSH`.
4. `VOXEL_IOC_FLIP_ASYNC` / `VOXEL_IOC_WAIT_FLIP`, which use `CONTROL.FLP` and `STATUS.VSY`.

## Register Behavior

The RTL word-addressed register constants live in `hw/voxel_gpu/rtl/voxel_gpu.sv` as `ADDR_*`. The C byte offsets live in `sw/voxel_gpu.h` as `VOXEL_REG_*`. The Avalon slave is declared in `hw/voxel_gpu_hw.tcl` with `addressUnits WORDS`, so C byte offset `0x0038` corresponds to RTL word address `ADDR_BAND_CTRL = 13'h00E`.

`STATUS` exposes `engine_busy`, FIFO full/empty/count, and a latched vsync bit. The driver polls `STATUS.FEM` and `STATUS.BSY` before flips and band transitions; no interrupt path is visible in the top-level RTL.

## Data Movement

Userspace owns world/chunk data and descriptor construction. The kernel driver owns only synchronization and a staging bounce buffer; it does not parse descriptor contents. The FPGA owns descriptor fetch, raster setup, pixel pipeline, color/Z band caches, SDRAM copy/scanout arbitration, and VGA output.

## Current Evidence and Limits

- Source confirms `/dev/voxel_gpu`, ioctl controls, FIFO streaming, banded rendering, SDRAM-backed display, and VGA scanout paths.
- Not produced in this environment. See `build/diagrams/yosys_voxel_gpu.log` for the exact command/error.
- VCD produced at `build/diagrams/voxel_gpu.vcd`; WaveJSON/SVG timing assets are derived from captured simulator signals.
- Quartus timing/resource/utilization values require Quartus reports after a full build.
