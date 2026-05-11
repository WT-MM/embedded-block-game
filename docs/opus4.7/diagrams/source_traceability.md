# Source traceability

For every artifact in `docs/diagrams/`, this table lists the
authoritative source file(s) the artifact was built from. Anyone
reviewing the diagrams should be able to follow these references back
to ground truth in one hop.

## Generated artifacts

| Artifact | Generator | Reads |
|---|---|---|
| `voxel_gpu_pipeline.mmd` | `scripts/gen_pipeline_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (PIPELINE_STAGES, pipeline regs) |
| `voxel_gpu_module_hierarchy.mmd` | `scripts/gen_pipeline_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` instance regex matches |
| `voxel_gpu_control_fsm.mmd` | `scripts/gen_pipeline_diagram.py` | `engine_state_t` enum + `state <= ST_*` assignments |
| `voxel_gpu_datapath.mmd` | `scripts/gen_datapath_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` instance regex (lane0/lane1) |
| `voxel_gpu_timing_pipeline.wave.json` | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (PIPELINE_STAGES) |
| `voxel_gpu_timing_avalon.wave.json` | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu_hw.tcl`, `sw/voxel_gpu.h` |
| `voxel_gpu_timing_band.wave.json` | `scripts/gen_timing_diagram.py` | `sw/gpu_transport.c`, `sw/voxel_gpu.h`, `voxel_gpu.sv` |
| `voxel_gpu_timing_cache_flush.wave.json` | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (cache flush FSM) |
| `voxel_gpu_timing_scanout_load.wave.json` | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (scanout RD-FIFO) |
| `voxel_gpu_timing_sdram_write.wave.json` | `scripts/gen_timing_diagram.py` | `hw/sdram_local_test/command.v`, `Sdram_Params.h` |
| `voxel_gpu_timing_sdram_read.wave.json` | `scripts/gen_timing_diagram.py` | `hw/sdram_local_test/command.v`, `Sdram_Params.h` |
| `voxel_gpu_timing_vga.wave.json` | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu/rtl/voxel_vga_counters.sv` |
| `build/diagrams/voxel_gpu.json` | `scripts/gen_netlist_json.sh` (yosys preferred) → fallback `scripts/gen_pipeline_diagram.py --emit-structural-json` | `hw/voxel_gpu/rtl/*.sv`, `*.svh` |
| `build/diagrams/soc_system.json` | `scripts/gen_pipeline_diagram.py --emit-soc-system-json` | `hw/soc_system.qsys` |

## Static artifacts

| Artifact | Reads |
|---|---|
| `full_system_architecture.mmd` | `hw/soc_system.qsys`, `hw/soc_system_top.sv`, `sw/voxel_gpu.{c,h}` |
| `hps_fpga_ownership.mmd` | `sw/*`, `hw/voxel_gpu/rtl/*`, `hw/voxel_gpu_hw.tcl` |
| `hps_software_architecture.mmd` | `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c` |
| `hps_to_fpga_dataflow.mmd` | `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv` |
| `register_interface_flow.mmd` | `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv` |
| `soc_system_context.mmd` | `hw/soc_system.qsys` |
| `memory_and_buffer_ownership.mmd` | `hw/voxel_gpu/rtl/voxel_gpu.sv`, `voxel_sdp_ram.sv`, `voxel_texture_rom.sv`, `sw/voxel_gpu.h` |
| `game_to_pixels_flow.mmd` | `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`, `voxel_vga_counters.sv` |
| `register_map.md` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (ADDR_*), `sw/voxel_gpu.h` (VOXEL_REG_*) |
| `hardware_software_interface.md` | `sw/voxel_gpu.{c,h}`, `sw/gpu_transport.c`, `sw/renderer.c`, `hw/voxel_gpu_hw.tcl` |

## How to verify

- Pipeline stages are exactly the prefix names enumerated in
  `PIPELINE_STAGES` inside `scripts/gen_pipeline_diagram.py`, which
  mirror the comment block at the top of `voxel_gpu.sv`.
- Engine states are extracted directly from the `engine_state_t` enum
  in `voxel_gpu.sv`; transition edges come from `state <= ST_*`
  assignments, attributed to the enclosing `case (state)` arm by line
  scan. Edges that cannot be attributed are dropped from the diagram
  (we do not invent them).
- Register addresses are read from RTL localparams (`ADDR_*`) and
  cross-checked against the C header byte offsets (`VOXEL_REG_*`).
- Submodule instances are read directly from `voxel_gpu.sv`. Lane0 /
  lane1 distinction comes from instance names ending in `0` / `1` and
  the `_o` suffix convention in pipeline register names.

If any of these sources change, regenerate the diagrams with
`make diagrams` from the repo root.
