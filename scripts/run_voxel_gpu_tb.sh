#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/diagrams"
LOG="$BUILD_DIR/voxel_gpu_tb.log"
VCD="$BUILD_DIR/voxel_gpu.vcd"
VERILATOR_OBJ="$BUILD_DIR/verilator_obj"

mkdir -p "$BUILD_DIR"
cd "$ROOT"
rm -f "$VCD"

RTL_FILES=(
  tb/voxel_gpu_tb.sv
  tb/sim_stubs.sv
  hw/voxel_gpu/rtl/voxel_gpu.sv
  hw/voxel_gpu/rtl/voxel_raster_math.sv
  hw/voxel_gpu/rtl/voxel_recip_math.sv
  hw/voxel_gpu/rtl/voxel_fog_blend.sv
  hw/voxel_gpu/rtl/voxel_sdp_ram.sv
  hw/voxel_gpu/rtl/voxel_texture_rom.sv
  hw/voxel_gpu/rtl/voxel_vga_counters.sv
  hw/sdram_local_test/Sdram_Control.v
  hw/sdram_local_test/control_interface.v
  hw/sdram_local_test/command.v
  hw/sdram_local_test/sdr_data_path.v
  hw/sdram_local_test/Sdram_RD_FIFO.v
  hw/sdram_local_test/Sdram_WR_FIFO.v
  hw/sdram_local_test/sdram_pll0.v
  hw/sdram_local_test/sdram_pll0/sdram_pll0_0002.v
)

if command -v iverilog >/dev/null 2>&1 && command -v vvp >/dev/null 2>&1; then
  {
    echo "Running voxel_gpu_tb with Icarus Verilog."
    iverilog -g2012 \
      -Ihw -Ihw/voxel_gpu/rtl -Ihw/sdram_local_test \
      -o "$BUILD_DIR/voxel_gpu_tb.vvp" \
      "${RTL_FILES[@]}"
    (cd hw && vvp "../build/diagrams/voxel_gpu_tb.vvp" +vcd=../build/diagrams/voxel_gpu.vcd)
  } > "$LOG" 2>&1
elif command -v verilator >/dev/null 2>&1; then
  rm -rf "$VERILATOR_OBJ"
  {
    echo "Running voxel_gpu_tb with Verilator."
    verilator --binary --timing --trace \
      --Mdir "$VERILATOR_OBJ" \
      --top-module voxel_gpu_tb \
      -Wno-fatal \
      -Wno-DECLFILENAME \
      -Wno-PINCONNECTEMPTY \
      -Wno-PINMISSING \
      -Wno-VARHIDDEN \
      -Wno-DEFPARAM \
      -Wno-WIDTHEXPAND \
      -Wno-WIDTHTRUNC \
      -Wno-UNSIGNED \
      -Wno-MULTIDRIVEN \
      -Wno-UNUSEDSIGNAL \
      -Wno-UNDRIVEN \
      -Wno-TIMESCALEMOD \
      -Ihw -Ihw/voxel_gpu/rtl -Ihw/sdram_local_test \
      "${RTL_FILES[@]}"
    (cd hw && "$VERILATOR_OBJ/Vvoxel_gpu_tb" +vcd=../build/diagrams/voxel_gpu.vcd)
  } > "$LOG" 2>&1
else
  {
    echo "INFO: no supported RTL simulator found."
    echo
    echo "Install one of:"
    echo "  - iverilog and vvp"
    echo "  - verilator"
  } > "$LOG"
  exit 127
fi

if [ ! -s "$VCD" ]; then
  {
    echo
    echo "ERROR: simulation completed but did not create $VCD"
  } >> "$LOG"
  exit 1
fi

echo "Generated $VCD" >> "$LOG"
