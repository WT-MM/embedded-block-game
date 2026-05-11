# Voxel GPU RTL

This directory owns the Platform Designer `voxel_gpu` component implementation.
The public top-level module is still `voxel_gpu`; the layout just keeps the
component RTL, generated assets, and asset generators together.

## Layout

- `rtl/voxel_gpu.sv` - top-level Avalon/VGA/SDRAM GPU peripheral.
- `rtl/voxel_math_utils.sv` - stateless raster setup, draw-step, reciprocal,
  and fog/translucency math modules.
- `rtl/voxel_perf_counters.sv` - frame performance counter block.
- `rtl/voxel_sdp_ram.sv` - explicit M10K simple dual-port RAM wrapper.
- `rtl/voxel_banked_sdp_ram.sv` - even/odd banked wrapper for band caches.
- `rtl/voxel_texture_rom.sv` - explicit M10K texture atlas ROM.
- `rtl/voxel_vga_counters.sv` - 640x480 VGA timing generator.
- `assets/textures.mif` - generated texture atlas for synthesis and virtual hardware.
- `assets/recip_lut.hex` - generated reciprocal lookup table for the raster pipeline.
- `assets/textures_preview.png` - human preview of the generated texture atlas.
- `scripts/` - generators for the files under `assets/`.

The original design document decomposed the GPU into more modules than the
current implementation exposes. The present hardware has since grown an
SDRAM-backed RGB565 display path, so this split follows the current RTL first
and the document's intent second: keep each authored SystemVerilog module in
its own file and keep the whole GPU component under one directory.
