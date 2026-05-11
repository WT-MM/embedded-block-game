# Source Traceability

## `c_call_graph.mmd`
- `sw/game.c`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`

## `full_system_architecture.mmd`
- `hw/soc_system.qsys`
- `hw/soc_system_top.sv`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`
- `sw/game.c`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`

## `game_to_pixels_flow.mmd`
- `sw/game.c`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `hps_fpga_ownership.mmd`
- `sw/world.h`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `hps_software_architecture.mmd`
- `sw/game.c`
- `sw/world.h`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`

## `hps_to_fpga_dataflow.mmd`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `sw/voxel_gpu.c`
- `sw/voxel_gpu.h`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `memory_and_buffer_ownership.mmd`
- `sw/world.h`
- `sw/renderer.c`
- `sw/gpu_transport.c`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `register_interface_flow.mmd`
- `sw/voxel_gpu.h`
- `sw/voxel_gpu.c`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `register_map.md`
- `sw/voxel_gpu.h`
- `sw/voxel_gpu.c`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `soc_system_context.mmd`
- `hw/soc_system.qsys`
- `hw/soc_system_top.sv`
- `hw/voxel_gpu_hw.tcl`

## `voxel_gpu_control_fsm.mmd`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`

## `voxel_gpu_datapath.mmd`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`
- `hw/voxel_gpu/rtl/voxel_raster_math.sv`

## `voxel_gpu_module_hierarchy.mmd`
- `hw/voxel_gpu/rtl/*.sv`
- `hw/voxel_gpu_hw.tcl`

## `voxel_gpu_pipeline.mmd`
- `hw/voxel_gpu/rtl/voxel_gpu.sv`
- `hw/voxel_gpu/rtl/voxel_recip_math.sv`

## `voxel_gpu_timing.svg`
- `docs/diagrams/voxel_gpu_timing.wave.json`
- `scripts/render_timing_diagram.py`

## `voxel_gpu_timing.wave.json`
- `tb/voxel_gpu_tb.sv`
- `build/diagrams/voxel_gpu.vcd when present`

Extraction notes:

- RTL register and FSM facts come from `hw/voxel_gpu/rtl/voxel_gpu.sv`.
- Software interface facts come from `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `sw/gpu_transport.c`, and `sw/renderer.c`.
- Platform Designer facts come from `hw/soc_system.qsys`, `hw/soc_system_top.sv`, and `hw/voxel_gpu_hw.tcl`.
- Timing behavior is not invented; the WaveDrom file is marked as a skeleton unless a VCD exists.
