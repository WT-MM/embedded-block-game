#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/diagrams"
VOXEL_LOG="$BUILD_DIR/yosys_voxel_gpu.log"
SOC_LOG="$BUILD_DIR/yosys_soc_system.log"

mkdir -p "$BUILD_DIR"

cd "$ROOT"

if ! grep -RqsE '^[[:space:]]*module[[:space:]]+voxel_gpu\b' hw/; then
  echo "ERROR: voxel_gpu module not found under hw/." | tee "$VOXEL_LOG" >&2
  exit 2
fi

if ! grep -RqsE '(^[[:space:]]*module[[:space:]]+soc_system\b|<module name="voxel_gpu_0"|soc_system soc_system0)' hw/; then
  echo "ERROR: soc_system context not found under hw/." | tee "$SOC_LOG" >&2
  exit 2
fi

if ! command -v yosys >/dev/null 2>&1; then
  {
    echo "ERROR: yosys not found on PATH."
    echo
    echo "Install Yosys, then rerun:"
    echo "  scripts/gen_netlist_json.sh"
    echo
    echo "Expected primary output:"
    echo "  build/diagrams/voxel_gpu.json"
  } | tee "$VOXEL_LOG" >&2
  exit 127
fi

STUBS="$BUILD_DIR/yosys_vendor_stubs.sv"
cat > "$STUBS" <<'STUBS_SV'
(* blackbox *) module altsyncram (
    input [31:0] address_a,
    input [31:0] address_b,
    input clock0,
    input clock1,
    input [255:0] data_a,
    input [255:0] data_b,
    input wren_a,
    input wren_b,
    input rden_a,
    input rden_b,
    input aclr0,
    input aclr1,
    input addressstall_a,
    input addressstall_b,
    input byteena_a,
    input byteena_b,
    input clocken0,
    input clocken1,
    input clocken2,
    input clocken3,
    output [255:0] q_a,
    output [255:0] q_b,
    output [2:0] eccstatus
);
endmodule

(* blackbox *) module dcfifo (
    input aclr,
    input [255:0] data,
    input rdclk,
    input rdreq,
    input wrclk,
    input wrreq,
    output [255:0] q,
    output rdempty,
    output rdfull,
    output wrempty,
    output wrfull,
    output [31:0] rdusedw,
    output [31:0] wrusedw,
    output [2:0] eccstatus
);
endmodule

(* blackbox *) module altera_pll (
    input refclk,
    input rst,
    output outclk_0,
    output locked
);
endmodule
STUBS_SV

mapfile -t RTL_FILES < <(
  find hw -type f \( -name '*.sv' -o -name '*.v' \) \
    ! -path '*/soc_system/synthesis/*' \
    ! -path '*/output_files/*' \
    | sort
)

if [ "${#RTL_FILES[@]}" -eq 0 ]; then
  echo "ERROR: no RTL files found under hw/." | tee "$VOXEL_LOG" >&2
  exit 2
fi

{
  echo "Yosys voxel_gpu JSON generation"
  echo "Root: $ROOT"
  echo "RTL files:"
  printf '  %s\n' "${RTL_FILES[@]}"
  echo
  echo "Command: yosys -l $VOXEL_LOG -p read_verilog/hierarchy/proc/memory/opt/write_json"
} > "$VOXEL_LOG"

yosys -l "$VOXEL_LOG" -p "
  read_verilog -sv -Ihw/voxel_gpu/rtl -Ihw/sdram_local_test $STUBS ${RTL_FILES[*]};
  hierarchy -check -top voxel_gpu;
  proc;
  memory;
  opt;
  write_json $BUILD_DIR/voxel_gpu.json
"

if [ -f hw/soc_system/synthesis/soc_system.v ]; then
  {
    echo "Yosys soc_system JSON generation"
    echo "Using generated Platform Designer HDL under hw/soc_system/synthesis."
  } > "$SOC_LOG"
  mapfile -t SOC_FILES < <(find hw/soc_system/synthesis -type f \( -name '*.v' -o -name '*.sv' \) | sort)
  yosys -l "$SOC_LOG" -p "
    read_verilog -sv -Ihw/soc_system/synthesis ${SOC_FILES[*]};
    hierarchy -check -top soc_system;
    proc;
    memory;
    opt;
    write_json $BUILD_DIR/soc_system.json
  "
else
  {
    echo "soc_system.json not generated."
    echo "Reason: hw/soc_system/synthesis/soc_system.v is not present."
    echo "Action: run Platform Designer generation from hw/ (for example, make -C hw qsys) and rerun scripts/gen_netlist_json.sh."
  } | tee "$SOC_LOG" >&2
fi

echo "Generated $BUILD_DIR/voxel_gpu.json"
