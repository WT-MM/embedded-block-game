# Final breakdown — voxel block game on Cyclone V SoC

A factual, source-grounded walkthrough of how the system is built.
Every assertion below is tied back to a file in the repository; if you
want to follow the chain end-to-end, the per-section "Source" lines
point at the canonical RTL or C file.

---

## 1. What the system does

A first-person voxel ("Minecraft-style") block game that runs on the
DE1-SoC's Cyclone V SoC. The HPS (ARM Cortex-A9 running Linux)
generates the world, meshes visible faces, and emits a flat stream of
quad descriptors. A custom FPGA peripheral (`voxel_gpu`) rasterizes
those quads to a 640×480 RGB565 framebuffer in board SDR SDRAM, then
scans the framebuffer out through a VGA DAC.

Source: `sw/game.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`,
`hw/soc_system.qsys`.

## 2. System partitioning

The split between HPS and FPGA is set by `hw/soc_system.qsys`. The
top-level diagram is in `docs/diagrams/full_system_architecture.mmd`.

| Layer | Owner | Lives in |
|---|---|---|
| World, simulation, input, UI | HPS (user) | `sw/world*.c`, `sw/player_physics.c`, `sw/game*.c`, `sw/input.c`, `sw/inventory.c`, `sw/chat.c` |
| Mesh generation + culling | HPS (user) | `sw/mesh_worker.c`, `sw/gen_worker.c` |
| Quad descriptor stream | HPS (user) | `sw/renderer.c`, `sw/gpu_transport.c` |
| Linux kernel ↔ device bridge | HPS (kernel) | `sw/voxel_gpu.c`, `sw/voxel_gpu.h` |
| Avalon-MM register decode + FIFO | FPGA | `hw/voxel_gpu/rtl/voxel_gpu.sv` |
| Rasterization (edge/Z/UV + 1/w) | FPGA | `voxel_raster_math.sv`, `voxel_recip_math.sv` |
| Texture sampling, palette, fog | FPGA | `voxel_texture_rom.sv`, `voxel_fog_blend.sv` |
| Band cache (on-chip M10K) | FPGA | `voxel_sdp_ram.sv` |
| SDRAM-backed double FB | FPGA | `hw/sdram_local_test/Sdram_Control.v` |
| VGA timing | FPGA | `voxel_vga_counters.sv` |

## 3. Platform Designer system (soc_system)

`hw/soc_system.qsys` instantiates five modules:

- `clk_0` — 50 MHz `clock_source`.
- `sdram_clocks` — `altera_up_avalon_sys_sdram_pll` (derives sys_clk +
  sdram_clk).
- `hps_0` — `altera_hps` v19.1.
- `fpga_sdram` — `altera_avalon_new_sdram_controller` (Micron
  MT48LC4M32B2, 16-bit, CAS=3 from the .qsys parameters).
- `voxel_gpu_0` — custom component (`voxel_gpu`, v1.0) registered via
  `hw/voxel_gpu_hw.tcl`.

Connections (also in `build/diagrams/soc_system.json`):

- `hps_0.h2f_axi_master  → fpga_sdram.s1`
- `hps_0.h2f_lw_axi_master → voxel_gpu_0.avalon_slave_0`
- `clk_0.clk → sdram_clocks.ref_clk`
- `sdram_clocks.sys_clk → fpga_sdram.clk / hps_0.f2h_axi_clock / hps_0.h2f_axi_clock / hps_0.h2f_lw_axi_clock / voxel_gpu_0.clock`

Conduits exported to `soc_system_top.sv`: `vga`, `voxel_sdram`,
`fpga_sdram_wire`, `hps_io`, `hps_ddr3`.

`soc_system.v` itself is produced by `qsys-generate` (see
`hw/Makefile` target `qsys`) and is not committed to this repo. The
.qsys file is the source of truth.

## 4. The voxel_gpu peripheral

### 4.1 Avalon-MM lightweight slave

From `hw/voxel_gpu_hw.tcl`:

- `addressUnits = WORDS`
- `address` width = 13, so the register window is 2^13 = 8192 32-bit
  words = 32 KB.
- 32-bit `writedata` + 4-bit `byteenable`; 32-bit `readdata`.
- `readWaitTime = 1`, `writeWaitTime = 0`. No bursts, no waitrequest.

The full register layout is in
[`docs/diagrams/register_map.md`](diagrams/register_map.md).

### 4.2 FIFO_WINDOW

Words 0x400..0x7FF (bytes 0x1000..0x1FFF) act as a 4 KB write-only
window into the on-chip descriptor FIFO. The FIFO itself is
`fifo_mem`, a 1024×32 array attributed `ramstyle = "M10K"` in
`voxel_gpu.sv`. Writes that land in the window are pushed into the
FIFO regardless of the low-order address bits.

### 4.3 Engine FSM

`engine_state_t` in `voxel_gpu.sv` enumerates 16 states (4-bit). The
main flow is:

```
IDLE -> FETCH -> SETUP -> DRAW -> DRAW_FLUSH -> IDLE
```

with side loops for cache management:

```
CACHE_EVICT -> CACHE_FLUSH_COLOR -> CACHE_FLUSH_Z
CACHE_SELECT_FILL -> CACHE_INIT / CACHE_LOAD_COLOR / CACHE_LOAD_Z
CACHE_DRAIN_COLOR / CACHE_DRAIN_Z
```

(see `docs/diagrams/voxel_gpu_control_fsm.mmd`).

### 4.4 Pixel pipeline (13 stages, 2 px/cycle)

The pipeline is fully registered between stages and processes two
adjacent x-pixels (lane0 = even x, lane1 = odd x) per clock. The
canonical stage list is the comment header inside `voxel_gpu.sv` and
the `PIPELINE_STAGES` constant in
`scripts/gen_pipeline_diagram.py`:

```
S0  pipe0     edge-test setup + UV/iw extraction
S1  recip0    iw normalize, MSB extraction
S2  recip1    1/iw LUT fetch (lo, hi entries)
S3  recip2    linear interpolate (LUT lo, hi, frac)
S4  pipe1     w denormalize -> per-pixel w (Q16.16)
S5  tex0      u = uw*w, v = vw*w (perspective-correct UV)
S6  pipe2     texture address build (tile, u, v)
S7  draw_pipe texture ROM read, light bank, dst Z/RGB fetch
S8  pal_rd    palette address register
S9  plr       palette[] read -> 24-bit RGB, RGB->RGB565
S10 fog0      radial distance: r^2/f^2, ray scale
S11 fog1      voxel_fog_blend (fog + alpha blend)
S12 commit    z-test, color/Z write to band cache
```

Combinational helpers live in submodules:

- `voxel_raster_setup` — edge equations + Z/UV starts.
- `voxel_draw_step` — lane1 plus next-pair/row deltas (the heavy
  64-bit multiplies and additions).
- `voxel_iw_normalize`, `voxel_recip_interpolate`, `voxel_w_denormalize`
  — two instances each (lane0/lane1) for the 1/w pipeline.
- `voxel_fog_blend` — two instances (lane0/lane1).
- `voxel_texture_rom` — 2× `altsyncram` ROM (port A = lane0, port B =
  lane1) sharing `voxel_gpu/assets/textures.mif`.

See `docs/diagrams/voxel_gpu_pipeline.mmd` and
`docs/diagrams/voxel_gpu_datapath.mmd`.

### 4.5 Band cache and SDRAM flush

`voxel_gpu.sv` instantiates four `voxel_banked_sdp_ram` blocks
(`fb_back_ram_A`, `fb_back_ram_B`, `z_ram_A`, `z_ram_B`). Each cache
is banked on `addr[0]` so even and odd x land in disjoint M10Ks; the
2 px/cycle pipeline drives the even and odd banks independently in
the same clock.

A render frame walks all 8 bands (480 / 60 = 8). Per band:

1. `BAND_CTRL.BEGIN` initiates `ST_CACHE_SELECT_FILL`. If the band has
   never been written this frame, the cache is initialized to clear
   color/Z; otherwise it is filled from SDRAM via `Sdram_Control`.
2. Descriptors stream in from the FIFO, ST_FETCH unpacks them, the
   pipeline draws into the cache.
3. `BAND_CTRL.FLUSH` walks `ST_CACHE_FLUSH_COLOR` / `_Z` to write the
   cache back to SDRAM in `COPY_BURST_WORDS = 128` word chunks. The
   write FIFO has a 224-word high-water mark
   (`COPY_WR_FIFO_HIGH_WATER`).

The framebuffer is double-buffered; `CONTROL.FLP` triggers a swap on
the next vsync (see the VGA section below).

### 4.6 VGA scan-out

`voxel_vga_counters.sv` generates 640×480 @ 25 MHz from the 50 MHz
fabric clock. `VGA_CLK = ~hcount[0]`, i.e. one VGA pixel every two
fabric clocks. Active horizontal counter values are 0..1279
(half-rate), so `hcount[10:1]` is the visible x. Sync widths
(matching the source file): HSYNC = 192 half-cycles, VSYNC = 2 lines,
front/back porches as defined at the top of
`voxel_vga_counters.sv`.

Scanout reads the front framebuffer through `Sdram_Control` (`RD_*`
ports). When `CONTROL.FLP` is pending and vsync arrives, the display
selector swaps which extmem base address is visible.

## 5. HPS software stack

### 5.1 User-space transport

`sw/gpu_transport.c` owns the per-frame data path:

- `gpu_transport_open()` opens `/dev/voxel_gpu`.
- `gpu_transport_begin_descriptors()` resets the per-band bins.
- The renderer appends `quad_desc` (and optional `quad_desc_uv`)
  records into the bin that covers the screen rows the quad touches.
- At frame end, each band's bin is shipped through
  `VOXEL_IOC_BEGIN_BAND` → `write(fd, bytes, len)` →
  `VOXEL_IOC_END_BAND`, then `VOXEL_IOC_FLIP` / `FLIP_ASYNC` flips the
  display.

### 5.2 Kernel driver

`sw/voxel_gpu.c` is a Linux platform-driver based misc device:

- `voxel_probe` `ioremap`s the register window using the device's
  `platform_get_resource(IORESOURCE_MEM, 0)`.
- `voxel_write` calls `iowrite32_rep(base + FIFO_BASE, src, words)`
  in chunks of up to FIFO free space, blocking on
  `voxel_fifo_wait_space` between chunks.
- `voxel_ioctl` switches on the 15 commands defined in
  `sw/voxel_gpu.h` and performs the corresponding MMIO writes /
  status polls.
- There are no IRQs; every wait is a STATUS poll
  (`voxel_poll_status`).

### 5.3 Renderer boundary

`sw/renderer.c::renderer_begin_frame` and `renderer_end_frame` mark
the transport boundary. Inside the frame, all `renderer_draw_*`
functions only enqueue descriptors; they perform no I/O. This keeps
the FPGA stream cleanly bracketed and lets the renderer also be
exercised by `sw/tests/renderer_*_test.c` without a real device.

## 6. Performance counters

The peripheral exposes ten free-running 32-bit cycle counters
(`PERF_DRAW_ACT`, `PERF_DRAW_IDLE`, `PERF_FLUSH_ACT`, `PERF_FLUSH_STL`,
`PERF_INIT`, `PERF_LOAD`, plus the four `PERF_FLUSH_WAIT_*` counters
behind `VOXEL_IOC_GET_PERF2`). They reset whenever software writes
`CONTROL.FLP`. `gpu_transport.c` reads the counters *before* issuing
FLIP, so the values describe the frame that just finished.

## 7. Tooling and reproducibility

The diagram + breakdown pipeline is one `make`-driven flow rooted in
the top-level `Makefile`:

```
make diagrams    # regenerate everything in docs/diagrams/
make breakdown   # rebuild markdown breakdowns (this file is hand-written;
                 # register_map.md and the diagram_index are regenerated)
```

The scripts that do the work are in `scripts/`. They are
intentionally small and source-grounded: every script lists the
inputs it reads in its module docstring. When Yosys is available on
the build machine, `gen_netlist_json.sh` runs Yosys against the
SystemVerilog tree (with `altsyncram` blackboxed) and emits a real
synthesized JSON netlist. On machines without Yosys (or when Yosys
cannot parse the SV/altsyncram surface), the script falls back to a
clearly marked source-parsed JSON.

Smoke simulation is provided by `tb/voxel_gpu_tb.sv`: a self-contained
testbench that drives clock + reset, issues a handful of Avalon-MM
writes to walk the engine FSM out of IDLE, tri-states the SDRAM
conduit safely, and dumps `voxel_gpu_tb.vcd`. It is not a correctness
test for raster output; it is a smoke harness that proves the DUT
elaborates and toggles.

## 8. Source-of-truth pointers

If you want to verify any claim in this document:

- Register addresses: `hw/voxel_gpu/rtl/voxel_gpu.sv` (ADDR_*) and
  `sw/voxel_gpu.h` (`VOXEL_REG_*`).
- Engine states: `engine_state_t` in `voxel_gpu.sv`.
- Pipeline stages: comment header + `pipeN_`, `recipN_`, `texN_`,
  `draw_pipe_`, `pal_rd_`, `plr_`, `fogN_`, `commit_` register
  prefixes in `voxel_gpu.sv`.
- Avalon attributes: `hw/voxel_gpu_hw.tcl`.
- Platform Designer system: `hw/soc_system.qsys`.
- VGA timing: `hw/voxel_gpu/rtl/voxel_vga_counters.sv`.
- HPS ↔ FPGA call path: `sw/game.c::main`, `sw/renderer.c::renderer_*`,
  `sw/gpu_transport.c`, `sw/voxel_gpu.c`.

For a one-page traceability map see
[`docs/diagrams/source_traceability.md`](diagrams/source_traceability.md).
