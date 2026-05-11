# Diagram index

Each diagram below is grounded in one or more source files. Generated
diagrams have a generator script; static diagrams were hand-written
against the same sources. View Mermaid diagrams with any Markdown
renderer that supports Mermaid (GitHub does), or paste into
<https://mermaid.live>. View the WaveDrom JSON at
<https://wavedrom.com/editor.html>.

## System-level (HPS + FPGA)

| File | Kind | Source of truth |
|---|---|---|
| [`full_system_architecture.mmd`](full_system_architecture.mmd) | static Mermaid | `hw/soc_system.qsys`, `hw/soc_system_top.sv`, `sw/voxel_gpu.{h,c}` |
| [`hps_fpga_ownership.mmd`](hps_fpga_ownership.mmd) | static Mermaid | `sw/*`, `hw/voxel_gpu/rtl/*` |
| [`soc_system_context.mmd`](soc_system_context.mmd) | static Mermaid | `hw/soc_system.qsys` |
| [`memory_and_buffer_ownership.mmd`](memory_and_buffer_ownership.mmd) | static Mermaid | `hw/voxel_gpu/rtl/voxel_gpu.sv` (M10K/MLAB attrs), `voxel_sdp_ram.sv`, `voxel_texture_rom.sv`, `sw/voxel_gpu.h` |

## Software (HPS side)

| File | Kind | Source of truth |
|---|---|---|
| [`hps_software_architecture.mmd`](hps_software_architecture.mmd) | static Mermaid | `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c` |
| [`hps_to_fpga_dataflow.mmd`](hps_to_fpga_dataflow.mmd) | static Mermaid | `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv` |
| [`register_interface_flow.mmd`](register_interface_flow.mmd) | static Mermaid | `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv` |
| [`game_to_pixels_flow.mmd`](game_to_pixels_flow.mmd) | static Mermaid | `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`, `voxel_vga_counters.sv` |

## FPGA (voxel_gpu) internals

| File | Kind | Generator | Source of truth |
|---|---|---|---|
| [`voxel_gpu_pipeline.mmd`](voxel_gpu_pipeline.mmd) | generated Mermaid | `scripts/gen_pipeline_diagram.py` | `hw/voxel_gpu/rtl/voxel_gpu.sv` (PIPELINE_STAGES + pipeline regs) |
| [`voxel_gpu_module_hierarchy.mmd`](voxel_gpu_module_hierarchy.mmd) | generated Mermaid | `scripts/gen_pipeline_diagram.py` | submodule instances in `voxel_gpu.sv` |
| [`voxel_gpu_control_fsm.mmd`](voxel_gpu_control_fsm.mmd) | generated Mermaid | `scripts/gen_pipeline_diagram.py` | `engine_state_t` + `state <= ST_X` edges |
| [`voxel_gpu_datapath.mmd`](voxel_gpu_datapath.mmd) | generated Mermaid | `scripts/gen_datapath_diagram.py` | submodule instances (`lane0`/`lane1`) in `voxel_gpu.sv` |
| [`voxel_gpu_timing_pipeline.wave.json`](voxel_gpu_timing_pipeline.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `voxel_gpu.sv` (PIPELINE_STAGES) |
| [`voxel_gpu_timing_avalon.wave.json`](voxel_gpu_timing_avalon.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu_hw.tcl`, `sw/voxel_gpu.h` |
| [`voxel_gpu_timing_band.wave.json`](voxel_gpu_timing_band.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `sw/gpu_transport.c`, `sw/voxel_gpu.h`, `voxel_gpu.sv` |
| [`voxel_gpu_timing_cache_flush.wave.json`](voxel_gpu_timing_cache_flush.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `voxel_gpu.sv` (cache flush FSM, `sdram_wr_*`) |
| [`voxel_gpu_timing_scanout_load.wave.json`](voxel_gpu_timing_scanout_load.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `voxel_gpu.sv` (`sdram_rd_*`, scanout) |
| [`voxel_gpu_timing_sdram_write.wave.json`](voxel_gpu_timing_sdram_write.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `hw/sdram_local_test/command.v`, `Sdram_Params.h` |
| [`voxel_gpu_timing_sdram_read.wave.json`](voxel_gpu_timing_sdram_read.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `hw/sdram_local_test/command.v`, `Sdram_Params.h` |
| [`voxel_gpu_timing_vga.wave.json`](voxel_gpu_timing_vga.wave.json) | generated WaveDrom | `scripts/gen_timing_diagram.py` | `hw/voxel_gpu/rtl/voxel_vga_counters.sv` |

## Structural JSON (Yosys / Platform Designer)

| File | Generator | Notes |
|---|---|---|
| `build/diagrams/voxel_gpu.json` | `scripts/gen_netlist_json.sh` then `gen_pipeline_diagram.py --emit-structural-json` | If Yosys is installed and accepts the SystemVerilog tree, it writes a real synthesized netlist. Otherwise (the default on this dev machine) we emit a source-parsed structural extract clearly marked with a `_blocker` field. |
| `build/diagrams/soc_system.json` | `scripts/gen_pipeline_diagram.py --emit-soc-system-json` | Parsed from `hw/soc_system.qsys`; the generated `soc_system.v` is a qsys-generate artifact and is not committed. |

## Markdown breakdowns

| File | Source of truth |
|---|---|
| [`register_map.md`](register_map.md) | `hw/voxel_gpu/rtl/voxel_gpu.sv` + `sw/voxel_gpu.h` |
| [`hardware_software_interface.md`](hardware_software_interface.md) | `sw/voxel_gpu.{c,h}`, `sw/gpu_transport.c`, `sw/renderer.c`, `hw/voxel_gpu_hw.tcl` |
| [`source_traceability.md`](source_traceability.md) | this directory's diagrams |
