# Diagram Index

Readable rendered gallery: `docs/diagrams/index.html`.
Single Markdown bundle: `docs/diagrams/all_diagrams.md`.

## `c_call_graph.mmd`

- Answers: Main C call path from game loop to hardware access functions.
- Based on: `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `full_system_architecture.mmd`

- Answers: Where the HPS software, Platform Designer system, voxel_gpu, SDRAM, and VGA output fit.
- Based on: `hw/soc_system.qsys`, `hw/soc_system_top.sv`, `hw/voxel_gpu/rtl/voxel_gpu.sv`, `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `game_to_pixels_flow.mmd`

- Answers: End-to-end game/world-to-VGA explanation.
- Based on: `sw/game.c`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `hps_fpga_ownership.mmd`

- Answers: Which side owns world/chunk data, descriptor buffers, CSRs, FIFOs, caches, and output memories.
- Based on: `sw/world.h`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `hps_software_architecture.mmd`

- Answers: How game.c, renderer.c, gpu_transport.c, world.c, and the kernel driver cooperate.
- Based on: `sw/game.c`, `sw/world.h`, `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `hps_to_fpga_dataflow.mmd`

- Answers: How world/chunk data becomes descriptors and crosses into voxel_gpu.
- Based on: `sw/renderer.c`, `sw/gpu_transport.c`, `sw/voxel_gpu.c`, `sw/voxel_gpu.h`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `memory_and_buffer_ownership.mmd`

- Answers: Who writes and reads each important memory/buffer.
- Based on: `sw/world.h`, `sw/renderer.c`, `sw/gpu_transport.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `register_interface_flow.mmd`

- Answers: Which C APIs/macros drive which RTL registers/signals.
- Based on: `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `register_map.md`

- Answers: The extracted hardware/software register map.
- Based on: `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: bit fields are limited to fields named in source comments/macros/RTL readback.

## `soc_system_context.mmd`

- Answers: The Platform Designer placement of voxel_gpu and HPS bridges.
- Based on: `hw/soc_system.qsys`, `hw/soc_system_top.sv`, `hw/voxel_gpu_hw.tcl`
- Limitation: generated Platform Designer HDL is not present, so this uses `.qsys` and top-level exported conduits.

## `source_traceability.md`

- Answers: Source files used for each diagram.
- Based on: 
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `voxel_gpu_control_fsm.mmd`

- Answers: The engine_state_t state flow and key transition conditions.
- Based on: `hw/voxel_gpu/rtl/voxel_gpu.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `voxel_gpu_datapath.mmd`

- Answers: The readable datapath through fetch, setup, draw, cache, SDRAM, and VGA.
- Based on: `hw/voxel_gpu/rtl/voxel_gpu.sv`, `hw/voxel_gpu/rtl/voxel_raster_math.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `voxel_gpu_module_hierarchy.mmd`

- Answers: The major RTL modules instantiated by voxel_gpu.
- Based on: `hw/voxel_gpu/rtl/*.sv`, `hw/voxel_gpu_hw.tcl`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `voxel_gpu_pipeline.mmd`

- Answers: The actual named pipeline register progression from pipe0 through commit.
- Based on: `hw/voxel_gpu/rtl/voxel_gpu.sv`, `hw/voxel_gpu/rtl/voxel_recip_math.sv`
- Limitation: readable source-level diagram, not a full gate/netlist rendering.

## `voxel_gpu_timing.svg`

- Answers: Static rendered view of the WaveDrom timing asset.
- Based on: `docs/diagrams/voxel_gpu_timing.wave.json`, `scripts/render_timing_diagram.py`
- Limitation: VCD produced at `build/diagrams/voxel_gpu.vcd`; WaveJSON/SVG timing assets are derived from captured simulator signals.

## `voxel_gpu_timing.wave.json`

- Answers: WaveDrom skeleton or VCD-derived timing if a real VCD is present.
- Based on: `tb/voxel_gpu_tb.sv`, `build/diagrams/voxel_gpu.vcd when present`
- Limitation: VCD produced at `build/diagrams/voxel_gpu.vcd`; WaveJSON/SVG timing assets are derived from captured simulator signals.
