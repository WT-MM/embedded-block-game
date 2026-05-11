# Hardware ↔ software interface

This document describes how the HPS-side Linux software in `sw/` talks
to the FPGA `voxel_gpu` peripheral. It is grounded in five source files:

- `sw/voxel_gpu.h`        — register byte offsets, bit fields, ioctl numbers, struct layouts.
- `sw/voxel_gpu.c`        — kernel platform driver + `/dev/voxel_gpu`.
- `sw/gpu_transport.c`    — user-space transport (per-band bins, ioctl dispatch).
- `sw/renderer.c`         — `renderer_begin_frame` / `renderer_end_frame` boundary.
- `hw/voxel_gpu/rtl/voxel_gpu.sv` + `hw/voxel_gpu_hw.tcl` — Avalon-MM
  slave + register decoder + engine FSM.

## Bus shape

The peripheral is a Platform Designer component (`hw/voxel_gpu_hw.tcl`)
attached to the HPS `h2f_lw_axi_master` via `soc_system.qsys`. The
Avalon-MM lightweight slave is configured for:

- `addressUnits = WORDS`         — `address[12:0]` selects a 32-bit word.
- 32-bit `writedata` + 4-bit `byteenable`, 32-bit `readdata`.
- `readWaitTime = 1`, `writeWaitTime = 0`, no waitrequest, no bursts.

A single transaction is one fabric cycle of `chipselect & write` with
the destination word address in `address`. The driver streams long
descriptor blocks by writing back-to-back into the FIFO_WINDOW; the
HPS-side `iowrite32_rep` (`sw/voxel_gpu.c`) issues one MMIO write per
fabric cycle.

## Device boot path

1. `voxel_probe` (kernel) — matches the platform device by `compatible`
   string (see `voxel_of_match` in `sw/voxel_gpu.c`), calls
   `platform_get_resource`, then `ioremap`s the register window.
2. The kernel registers `/dev/voxel_gpu` as a misc-device with
   `voxel_fops`.
3. User-space opens `/dev/voxel_gpu` from
   `gpu_transport_open()` in `sw/gpu_transport.c`.

## Per-frame call path

The path from `sw/game.c` main loop to a VGA pixel:

```
game.c::main()
  └─ renderer_begin_frame(ctx)            // renderer.c
       └─ gpu_transport_begin_descriptors // gpu_transport.c
            └─ start per-band bins
  └─ renderer_draw_*                       // renderer.c
       └─ append quad_desc(+_uv) into bin for band(s) it covers
  └─ renderer_end_frame(ctx)              // renderer.c
       └─ for each band b in 0..7:
            ├─ ioctl VOXEL_IOC_BEGIN_BAND(b)
            ├─ write(fd, bin_bytes, bin_len)    // descriptors -> FIFO_WINDOW
            └─ ioctl VOXEL_IOC_END_BAND         // BAND_CTRL.FLUSH, poll BSY
       └─ gpu_transport_flip
            └─ ioctl VOXEL_IOC_FLIP_ASYNC or VOXEL_IOC_FLIP
                 └─ CONTROL.FLP set; HW swaps front/back on next vsync
```

The same descriptor stream that `renderer.c` builds is consumed by the
hardware's ST_FETCH state, which deserializes packed words into
`pipe0_*` register sets.

## Synchronization model

There is no IRQ. Synchronization is via polling. The kernel helpers
are:

- `voxel_poll_status(mask, expect, timeout_ms)` — waits for STATUS
  bits to settle. Used after every band flush and flip.
- `voxel_fifo_wait_space(space_words, min_req)` — waits for FIFO_COUNT
  in STATUS to fall below the threshold before pushing more
  descriptor words.

The relevant status bits are:

- `VOXEL_STAT_BSY`  — engine busy (FSM is in any non-IDLE state).
- `VOXEL_STAT_FFL`  — FIFO full.
- `VOXEL_STAT_FEM`  — FIFO empty.
- `VOXEL_STAT_VSY`  — vsync latched (cleared on read).
- `VOXEL_STAT_FIFO_COUNT[19:4]` — words currently buffered.

## Configuration registers in detail

CONTROL (word 0x000) — masks defined in `sw/voxel_gpu.h`:

- `EN`  enables the rasterizer FSM. Cleared after reset.
- `FLP` requests a flip on the next vsync.
- `IEN` is reserved for an interrupt-enable bit; the driver does not
  use IRQs today.
- `CLR` clears the active back framebuffer (color and depth).

BAND_CTRL (word 0x00E):

- `BEGIN` pulses ST_CACHE_SELECT_FILL → ST_CACHE_INIT/LOAD path to set
  up the band cache for the band specified by `BAND_INDEX`.
- `FLUSH` pulses ST_CACHE_FLUSH_COLOR / FLUSH_Z so the band cache is
  written back to SDRAM through `Sdram_Control`.

FOG_RANGE / FOG_CTRL:

- start/end distances are Q8.8 fixed-point (radial distance, not
  z-depth). `inv_proj_sq` is Q0.16, used to convert post-projective
  distance into radial space (see `voxel_fog_blend`).

EXTMEM_*:

- `EXTMEM_CTRL` selects RGB565 mode, scanout enable, back-buffer
  enable, and the experimental sky-gradient-clear / tile-cache bits.
- `EXTMEM_FRONT`/`EXTMEM_BACK` are byte addresses inside the board
  SDRAM where the front and back framebuffers live.
- `EXTMEM_STRIDE` is bytes per scanline (640 × 2 = 1280 for RGB565).

## Descriptor layout summary

The streaming format is the same on both ends; see
`docs/diagrams/register_map.md` for the byte-accurate description and
`sw/voxel_gpu.h` for the struct definitions. Sizes (asserted at
compile time by `_Static_assert`):

- `struct quad_desc`      — 64 bytes (color quad; always present).
- `struct quad_desc_uv`   — 36 bytes (textured-quad extension).
- `struct edge_coef`      — 12 bytes (one of four edge equations).

The hardware identifies a textured descriptor by `flags &
QUAD_FLAG_TEX`. When set, the next 9 words (36 bytes) hold the
perspective-correct UV plane equations.

## What is NOT in this interface

- No DMA: every descriptor word crosses the lightweight-AXI bridge as
  an explicit MMIO write.
- No shared memory between HPS DDR3 and the band caches: SDRAM-backed
  framebuffer traffic is on the FPGA-side SDR SDRAM controller, not
  HPS DDR3.
- No interrupts: every wait is a polled STATUS read.
