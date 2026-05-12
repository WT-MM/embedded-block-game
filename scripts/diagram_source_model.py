#!/usr/bin/env python3
"""Source-grounded diagram/document generation helpers.

The goal of this module is not to reverse-engineer every gate. It extracts the
stable facts that make human architecture diagrams useful: module names,
register definitions, Platform Designer connections, C hardware-access paths,
and the named voxel_gpu pipeline registers that are visible in the RTL.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import json
import re
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
DIAGRAMS = DOCS / "diagrams"
BUILD_DIAGRAMS = ROOT / "build" / "diagrams"


RTL = ROOT / "hw" / "voxel_gpu" / "rtl" / "voxel_gpu.sv"
RTL_GUIDE = ROOT / "hw" / "voxel_gpu" / "rtl" / "README.md"
VOXEL_TCL = ROOT / "hw" / "voxel_gpu_hw.tcl"
QSYS = ROOT / "hw" / "soc_system.qsys"
SOC_TOP = ROOT / "hw" / "soc_system_top.sv"
QSF = ROOT / "hw" / "soc_system.qsf"
SW_GPU_H = ROOT / "sw" / "voxel_gpu.h"
SW_GPU_C = ROOT / "sw" / "voxel_gpu.c"
SW_TRANSPORT_C = ROOT / "sw" / "gpu_transport.c"
SW_TRANSPORT_H = ROOT / "sw" / "gpu_transport.h"
SW_RENDERER_C = ROOT / "sw" / "renderer.c"
SW_RENDERER_H = ROOT / "sw" / "renderer.h"
SW_GAME_C = ROOT / "sw" / "game.c"
SW_WORLD_H = ROOT / "sw" / "world.h"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def ensure_dirs() -> None:
    DIAGRAMS.mkdir(parents=True, exist_ok=True)
    BUILD_DIAGRAMS.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.rstrip() + "\n", encoding="utf-8")


def all_source_files(patterns: Iterable[str]) -> list[Path]:
    files: list[Path] = []
    for pattern in patterns:
        files.extend(ROOT.glob(pattern))
    return sorted(p for p in files if p.is_file())


def extract_modules() -> dict[str, str]:
    modules: dict[str, str] = {}
    for path in all_source_files(["hw/**/*.sv", "hw/**/*.v"]):
        text = read(path)
        for match in re.finditer(r"(?m)^\s*module\s+([A-Za-z_]\w*)\b", text):
            modules[match.group(1)] = rel(path)
    return dict(sorted(modules.items()))


def extract_localparams() -> dict[str, int]:
    text = read(RTL)
    params: dict[str, int] = {}
    for name, value in re.findall(
        r"localparam\s+(?:logic\s+\[[^\]]+\]\s+|int\s+)?(ADDR_[A-Z0-9_]+)\s*=\s*13'h([0-9a-fA-F]+)",
        text,
    ):
        params[name] = int(value, 16)
    return dict(sorted(params.items(), key=lambda item: item[1]))


def extract_c_register_macros() -> dict[str, int]:
    text = read(SW_GPU_H)
    regs: dict[str, int] = {}
    for name, value in re.findall(r"#define\s+(VOXEL_REG_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)", text):
        regs[name] = int(value, 16)
    return dict(sorted(regs.items(), key=lambda item: item[1]))


def extract_fifo_window() -> tuple[int, int]:
    text = read(SW_GPU_H)
    base = re.search(r"#define\s+VOXEL_FIFO_BASE\s+(0x[0-9A-Fa-f]+)", text)
    end = re.search(r"#define\s+VOXEL_FIFO_END\s+(0x[0-9A-Fa-f]+)", text)
    return (int(base.group(1), 16), int(end.group(1), 16)) if base and end else (0x1000, 0x2000)


def rtl_addr_name_for_macro(macro: str) -> str | None:
    mapping = {
        "VOXEL_REG_CONTROL": "ADDR_CONTROL",
        "VOXEL_REG_STATUS": "ADDR_STATUS",
        "VOXEL_REG_FRAME_COUNT": "ADDR_FRAMECNT",
        "VOXEL_REG_PALETTE_ADDR": "ADDR_PAL_ADDR",
        "VOXEL_REG_PALETTE_DATA": "ADDR_PAL_DATA",
        "VOXEL_REG_FOG_RANGE": "ADDR_FOG_RANGE",
        "VOXEL_REG_FOG_CTRL": "ADDR_FOG_CTRL",
        "VOXEL_REG_EXTMEM_CTRL": "ADDR_EXTMEM_CTRL",
        "VOXEL_REG_EXTMEM_FRONT": "ADDR_EXTMEM_FRONT",
        "VOXEL_REG_EXTMEM_BACK": "ADDR_EXTMEM_BACK",
        "VOXEL_REG_EXTMEM_STRIDE": "ADDR_EXTMEM_STRIDE",
        "VOXEL_REG_EXTMEM_TILE": "ADDR_EXTMEM_TILE",
        "VOXEL_REG_EXTMEM_STAT": "ADDR_EXTMEM_STAT",
        "VOXEL_REG_BAND_INDEX": "ADDR_BAND_INDEX",
        "VOXEL_REG_BAND_CTRL": "ADDR_BAND_CTRL",
        "VOXEL_REG_BAND_WINDOW": "ADDR_BAND_WINDOW",
        "VOXEL_REG_PERF_DRAW_ACT": "ADDR_PERF_DRAW_ACT",
        "VOXEL_REG_PERF_DRAW_IDLE": "ADDR_PERF_DRAW_IDLE",
        "VOXEL_REG_PERF_FLUSH_ACT": "ADDR_PERF_FLUSH_ACT",
        "VOXEL_REG_PERF_FLUSH_STL": "ADDR_PERF_FLUSH_STL",
        "VOXEL_REG_PERF_INIT": "ADDR_PERF_INIT",
        "VOXEL_REG_PERF_LOAD": "ADDR_PERF_LOAD",
        "VOXEL_REG_PERF_FLUSH_LOAD": "ADDR_PERF_FLUSH_LOAD",
        "VOXEL_REG_PERF_FLUSH_FIFO": "ADDR_PERF_FLUSH_FIFO",
        "VOXEL_REG_PERF_FLUSH_DATA": "ADDR_PERF_FLUSH_DATA",
        "VOXEL_REG_PERF_FLUSH_DRAIN": "ADDR_PERF_FLUSH_DRAIN",
        "VOXEL_REG_SKY_PALETTE_ADDR": "ADDR_SKY_PAL_ADDR",
        "VOXEL_REG_SKY_PALETTE_DATA": "ADDR_SKY_PAL_DATA",
    }
    return mapping.get(macro)


def access_for_rtl_addr(addr_name: str) -> str:
    text = read(RTL)
    has_write = bool(re.search(rf"(?m)^\s*{re.escape(addr_name)}\s*:", text[: text.find("if (fifo_push_req)")]))
    read_case = text[text.find("always_comb begin\n        case (address)") :]
    has_read = bool(re.search(rf"(?m)^\s*{re.escape(addr_name)}\s*:", read_case))
    if has_read and has_write:
        return "R/W"
    if has_read:
        return "R"
    if has_write:
        return "W"
    return "unknown"


def extract_states() -> list[str]:
    text = read(RTL)
    match = re.search(r"typedef enum logic \[3:0\] \{(?P<body>.*?)\}\s+engine_state_t;", text, re.S)
    if not match:
        return []
    return re.findall(r"\b(ST_[A-Z0-9_]+)\b", match.group("body"))


def extract_instances() -> list[tuple[str, str]]:
    text = read(RTL)
    modules = [
        "voxel_raster_setup",
        "voxel_draw_step",
        "voxel_iw_normalize",
        "voxel_recip_interpolate",
        "voxel_w_denormalize",
        "voxel_fog_blend",
        "voxel_vga_counters",
        "Sdram_Control",
        "voxel_banked_sdp_ram",
        "voxel_texture_rom",
    ]
    insts: list[tuple[str, str]] = []
    for module in modules:
        pattern = rf"\b{re.escape(module)}\b\s*(?:#\s*\(.*?\)\s*)?([A-Za-z_]\w*)\s*\("
        for inst in re.findall(pattern, text, re.S):
            insts.append((module, inst))
    return sorted(insts)


def c_function_contexts(path: Path, tokens: Iterable[str]) -> dict[str, set[str]]:
    wanted = set(tokens)
    hits: dict[str, set[str]] = {token: set() for token in wanted}
    current = "file scope"
    brace_depth = 0
    pending_name: str | None = None
    func_start = re.compile(
        r"^\s*(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*\s+([A-Za-z_]\w+)\s*\([^;]*$"
    )
    one_line_start = re.compile(
        r"^\s*(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*\s+([A-Za-z_]\w+)\s*\([^;]*\)\s*\{"
    )
    for line in read(path).splitlines():
        one_line = one_line_start.match(line)
        if one_line and "if " not in line and "while " not in line and "for " not in line:
            current = one_line.group(1)
            brace_depth = 1
        elif brace_depth == 0:
            match = func_start.match(line)
            if match and not line.strip().startswith(("if", "for", "while", "switch")):
                pending_name = match.group(1)
        if pending_name and "{" in line:
            current = pending_name
            pending_name = None
            brace_depth = 1
        for token in wanted:
            if token in line:
                hits[token].add(f"{rel(path)}::{current}")
        if brace_depth > 0:
            brace_depth += line.count("{")
            brace_depth -= line.count("}")
            if brace_depth <= 0:
                brace_depth = 0
                current = "file scope"
    return hits


@dataclass(frozen=True)
class RegisterRow:
    offset: int
    name: str
    rtl_addr: str
    direction: str
    bit_fields: str
    reset: str
    c_usage: str
    hw_signal: str
    notes: str


def register_rows() -> list[RegisterRow]:
    macros = extract_c_register_macros()
    usages = c_function_contexts(SW_GPU_C, macros.keys())
    usage_overrides = {
        "VOXEL_REG_CONTROL": "voxel_ioc_clear(), voxel_ioc_flip_common(), voxel_probe(), voxel_remove()",
        "VOXEL_REG_STATUS": "voxel_status(), voxel_poll_status(), voxel_ioc_get_status()",
        "VOXEL_REG_FRAME_COUNT": "voxel_ioc_get_frame_count()",
        "VOXEL_REG_PALETTE_ADDR": "voxel_ioc_set_palette()",
        "VOXEL_REG_PALETTE_DATA": "voxel_ioc_set_palette()",
        "VOXEL_REG_FOG_RANGE": "voxel_ioc_set_fog()",
        "VOXEL_REG_FOG_CTRL": "voxel_ioc_set_fog()",
        "VOXEL_REG_EXTMEM_CTRL": "voxel_ioc_set_extmem(), voxel_ioc_get_extmem()",
        "VOXEL_REG_EXTMEM_FRONT": "voxel_ioc_set_extmem(), voxel_ioc_get_extmem()",
        "VOXEL_REG_EXTMEM_BACK": "voxel_ioc_set_extmem(), voxel_ioc_get_extmem()",
        "VOXEL_REG_EXTMEM_STRIDE": "voxel_ioc_set_extmem(), voxel_ioc_get_extmem()",
        "VOXEL_REG_EXTMEM_TILE": "voxel_ioc_set_extmem(), voxel_ioc_get_extmem()",
        "VOXEL_REG_EXTMEM_STAT": "voxel_ioc_get_extmem(); gpu_transport_read_copy_target_buffer(), log_extmem_state() via ioctl",
        "VOXEL_REG_BAND_INDEX": "voxel_ioc_begin_band()",
        "VOXEL_REG_BAND_CTRL": "voxel_ioc_begin_band(), voxel_ioc_end_band()",
        "VOXEL_REG_BAND_WINDOW": "voxel_ioc_begin_band()",
        "VOXEL_REG_PERF_DRAW_ACT": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_DRAW_IDLE": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_ACT": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_STL": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_INIT": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_LOAD": "voxel_ioc_get_perf(), voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_LOAD": "voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_FIFO": "voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_DATA": "voxel_ioc_get_perf2()",
        "VOXEL_REG_PERF_FLUSH_DRAIN": "voxel_ioc_get_perf2()",
        "VOXEL_REG_SKY_PALETTE_ADDR": "voxel_ioc_set_sky_palette()",
        "VOXEL_REG_SKY_PALETTE_DATA": "voxel_ioc_set_sky_palette()",
    }
    bit_fields = {
        "VOXEL_REG_CONTROL": "[0]=EN, [1]=FLP pulse, [2]=IEN, [3]=CLR pulse",
        "VOXEL_REG_STATUS": "[0]=BSY, [1]=FFL, [2]=FEM, [3]=VSY, [19:4]=FIFO_COUNT",
        "VOXEL_REG_FRAME_COUNT": "[31:0]=frame_count",
        "VOXEL_REG_PALETTE_ADDR": "[7:0]=palette write index",
        "VOXEL_REG_PALETTE_DATA": "[23:16]=R, [15:8]=G, [7:0]=B",
        "VOXEL_REG_FOG_RANGE": "[15:0]=start_dist, [31:16]=end_dist",
        "VOXEL_REG_FOG_CTRL": "[7:0]=palette fog color, [8]=enable, [31:16]=inv_proj_sq",
        "VOXEL_REG_EXTMEM_CTRL": "bits include ENABLE, SCANOUT_EN, BACKBUF_EN, RGB565, TILE_CACHE_EN, SKY_GRADIENT_CLEAR per sw/voxel_gpu.h",
        "VOXEL_REG_EXTMEM_FRONT": "[31:0]=front SDRAM byte base",
        "VOXEL_REG_EXTMEM_BACK": "[31:0]=back SDRAM byte base",
        "VOXEL_REG_EXTMEM_STRIDE": "[31:0]=bytes per scanline",
        "VOXEL_REG_EXTMEM_TILE": "[15:0]=tile_w, [31:16]=tile_h per header comment",
        "VOXEL_REG_EXTMEM_STAT": "RTL-packed SDRAM/display/cache status; field names not fully defined in sw/voxel_gpu.h",
        "VOXEL_REG_BAND_INDEX": "[2:0]=active 60-line band",
        "VOXEL_REG_BAND_CTRL": "[0]=BEGIN pulse, [1]=FLUSH pulse",
        "VOXEL_REG_BAND_WINDOW": "[5:0]=flush local y0, [13:8]=flush local y1",
        "VOXEL_REG_PERF_DRAW_ACT": "[31:0]=draw active cycles",
        "VOXEL_REG_PERF_DRAW_IDLE": "[31:0]=draw idle cycles",
        "VOXEL_REG_PERF_FLUSH_ACT": "[31:0]=background flush active cycles",
        "VOXEL_REG_PERF_FLUSH_STL": "[31:0]=background flush stalled cycles",
        "VOXEL_REG_PERF_INIT": "[31:0]=cache init cycles",
        "VOXEL_REG_PERF_LOAD": "[31:0]=reserved legacy cache-load counter; currently zero",
        "VOXEL_REG_PERF_FLUSH_LOAD": "[31:0]=flush wait for load cycles",
        "VOXEL_REG_PERF_FLUSH_FIFO": "[31:0]=flush wait for FIFO/headroom cycles",
        "VOXEL_REG_PERF_FLUSH_DATA": "[31:0]=flush wait for cache/sky data cycles",
        "VOXEL_REG_PERF_FLUSH_DRAIN": "[31:0]=flush final drain cycles",
        "VOXEL_REG_SKY_PALETTE_ADDR": "[4:0]=sky gradient palette index",
        "VOXEL_REG_SKY_PALETTE_DATA": "[23:16]=R, [15:8]=G, [7:0]=B",
    }
    reset = {
        "VOXEL_REG_CONTROL": "0x00000000",
        "VOXEL_REG_STATUS": "derived from reset state; FIFO empty and not busy after reset",
        "VOXEL_REG_FRAME_COUNT": "0x00000000",
        "VOXEL_REG_PALETTE_ADDR": "0x00000000",
        "VOXEL_REG_FOG_RANGE": "0x00000000",
        "VOXEL_REG_FOG_CTRL": "0x00000000",
        "VOXEL_REG_EXTMEM_CTRL": "DEFAULT_EXTMEM_CTRL = 0x0000002B",
        "VOXEL_REG_EXTMEM_FRONT": "DEFAULT_EXTMEM_FRONT_BASE = 0",
        "VOXEL_REG_EXTMEM_BACK": "DEFAULT_EXTMEM_BACK_BASE = 1048576",
        "VOXEL_REG_EXTMEM_STRIDE": "DEFAULT_EXTMEM_STRIDE = 1280",
        "VOXEL_REG_EXTMEM_TILE": "0x00000000",
        "VOXEL_REG_EXTMEM_STAT": "0x00000000",
        "VOXEL_REG_BAND_INDEX": "0",
        "VOXEL_REG_BAND_CTRL": "pending bits reset to 0",
        "VOXEL_REG_BAND_WINDOW": "y_min=0, y_max=59",
        "VOXEL_REG_SKY_PALETTE_ADDR": "0",
    }
    hw_signals = {
        "VOXEL_REG_CONTROL": "ctrl_en, ctrl_ien, ctrl_flp_pending, clear_pending",
        "VOXEL_REG_STATUS": "engine_busy, fifo_full, fifo_empty, vsy_latch, fifo_count",
        "VOXEL_REG_FRAME_COUNT": "frame_count",
        "VOXEL_REG_PALETTE_ADDR": "pal_addr",
        "VOXEL_REG_PALETTE_DATA": "palette[pal_addr]",
        "VOXEL_REG_FOG_RANGE": "fog_start_dist, fog_end_dist",
        "VOXEL_REG_FOG_CTRL": "fog_color, fog_enable, fog_inv_proj_sq",
        "VOXEL_REG_EXTMEM_CTRL": "extmem_ctrl",
        "VOXEL_REG_EXTMEM_FRONT": "extmem_front_base",
        "VOXEL_REG_EXTMEM_BACK": "extmem_back_base",
        "VOXEL_REG_EXTMEM_STRIDE": "extmem_stride_bytes",
        "VOXEL_REG_EXTMEM_TILE": "extmem_tile_cfg",
        "VOXEL_REG_EXTMEM_STAT": "extmem_dma_status",
        "VOXEL_REG_BAND_INDEX": "band_index_cfg",
        "VOXEL_REG_BAND_CTRL": "band_begin_pending, band_flush_pending",
        "VOXEL_REG_BAND_WINDOW": "band_flush_y_min_cfg, band_flush_y_max_cfg",
        "VOXEL_REG_PERF_DRAW_ACT": "perf_draw_active",
        "VOXEL_REG_PERF_DRAW_IDLE": "perf_draw_idle",
        "VOXEL_REG_PERF_FLUSH_ACT": "perf_flush_active",
        "VOXEL_REG_PERF_FLUSH_STL": "perf_flush_stall",
        "VOXEL_REG_PERF_INIT": "perf_init",
        "VOXEL_REG_PERF_LOAD": "perf_load",
        "VOXEL_REG_PERF_FLUSH_LOAD": "perf_flush_wait_load",
        "VOXEL_REG_PERF_FLUSH_FIFO": "perf_flush_wait_fifo",
        "VOXEL_REG_PERF_FLUSH_DATA": "perf_flush_wait_data",
        "VOXEL_REG_PERF_FLUSH_DRAIN": "perf_flush_wait_drain",
        "VOXEL_REG_SKY_PALETTE_ADDR": "sky_pal_addr",
        "VOXEL_REG_SKY_PALETTE_DATA": "sky_palette[sky_pal_addr]",
    }
    rows: list[RegisterRow] = []
    for macro, offset in macros.items():
        rtl_addr = rtl_addr_name_for_macro(macro) or "unknown"
        usage = usage_overrides.get(macro)
        if not usage:
            usage = ", ".join(sorted(usages.get(macro, []))) or "not found in sw/voxel_gpu.c scan"
        rows.append(
            RegisterRow(
                offset=offset,
                name=macro.removeprefix("VOXEL_REG_"),
                rtl_addr=rtl_addr,
                direction=access_for_rtl_addr(rtl_addr) if rtl_addr != "unknown" else "unknown",
                bit_fields=bit_fields.get(macro, "No bitfield comment found; see sw/voxel_gpu.h and RTL cases."),
                reset=reset.get(macro, "not explicitly documented; see reset block in hw/voxel_gpu/rtl/voxel_gpu.sv"),
                c_usage=usage,
                hw_signal=hw_signals.get(macro, "unknown"),
                notes="extracted from sw/voxel_gpu.h defines and hw/voxel_gpu/rtl/voxel_gpu.sv address cases",
            )
        )
    base, end = extract_fifo_window()
    rows.append(
        RegisterRow(
            offset=base,
            name=f"FIFO_WINDOW 0x{base:04X}..0x{end - 1:04X}",
            rtl_addr="ADDR_FIFO_LO..ADDR_FIFO_HI",
            direction="W",
            bit_fields="32-bit descriptor words; 4 KB / 1024 words",
            reset="fifo_count=0, fifo pointers=0",
            c_usage=f"{rel(SW_GPU_C)}::voxel_write via iowrite32_rep(), {rel(SW_TRANSPORT_C)}::submit_hw_band_flat via write()",
            hw_signal="fifo_mem, fifo_wr_ptr, fifo_rd_ptr, fifo_count",
            notes="driver requires 32-bit aligned writes; descriptor boundaries are owned by userspace",
        )
    )
    return sorted(rows, key=lambda row: row.offset)


def yosys_status() -> str:
    json_path = BUILD_DIAGRAMS / "voxel_gpu.json"
    log_path = BUILD_DIAGRAMS / "yosys_voxel_gpu.log"
    if json_path.exists():
        return f"Produced: `{rel(json_path)}`."
    if log_path.exists():
        return f"Not produced in this environment. See `{rel(log_path)}` for the exact command/error."
    return "Not produced yet. Run `make diagrams` or `scripts/gen_netlist_json.sh`."


def simulation_status() -> str:
    vcd = BUILD_DIAGRAMS / "voxel_gpu.vcd"
    if vcd.exists():
        return f"VCD produced at `{rel(vcd)}`; WaveJSON/SVG timing assets are derived from captured simulator signals."
    return "No VCD found; timing WaveDrom is a skeleton until a simulator produces `build/diagrams/voxel_gpu.vcd`."


def mermaid_header() -> str:
    return """%% Generated from RTL/C/Platform Designer source. Do not hand-edit generated diagrams.
%% Regenerate with: make diagrams
"""


def class_defs() -> str:
    return """
classDef hps fill:#e8f1ff,stroke:#2f63b8,stroke-width:1px,color:#0d2448;
classDef fpga fill:#e9f7ef,stroke:#2a7a45,stroke-width:1px,color:#0d341c;
classDef reg fill:#fff3cc,stroke:#b98500,stroke-width:1px,color:#3d2c00;
classDef mem fill:#f4e9ff,stroke:#6e3fa0,stroke-width:1px,color:#2d124a;
classDef bus fill:#ffe8df,stroke:#b55632,stroke-width:1px,color:#4a1f0b;
classDef ctrl fill:#ffe9ef,stroke:#b83a58,stroke-width:1px,color:#4a0d1b;
classDef math fill:#e6fbff,stroke:#247a91,stroke-width:1px,color:#09313d;
classDef out fill:#eeeeee,stroke:#555555,stroke-width:1px,color:#222222;
"""


def full_system_architecture() -> str:
    return mermaid_header() + """flowchart LR
subgraph HPS["HPS / Linux / C Software"]
  game["game.c main() loop"]:::hps
  world["VoxelWorld, Chunk, ChunkMesh"]:::hps
  input["input/chat/inventory/player update"]:::hps
  renderer["renderer.c<br/>RenderContext + quad descriptors"]:::hps
  transport["gpu_transport.c<br/>band binning + /dev/voxel_gpu"]:::hps
  driver["voxel_gpu.c kernel misc driver<br/>ioctl(), write(), iowrite32_rep()"]:::hps
end
subgraph PD["Platform Designer soc_system boundary"]
  hps0["hps_0"]:::bus
  lw["h2f_lw_axi_master<br/>base 0x0000"]:::bus
  h2f["h2f_axi_master<br/>base 0x0000"]:::bus
  fpga_sdram["fpga_sdram.s1<br/>Avalon SDRAM controller"]:::mem
  pll["sdram_clocks.sys_clk"]:::ctrl
  subgraph FABRIC["FPGA Fabric"]
    gpu["voxel_gpu_0 / voxel_gpu<br/>Avalon-MM slave + raster engine"]:::fpga
    fifo["descriptor FIFO<br/>fifo_mem[1024]"]:::mem
    caches["ping-pong 640x60 color/Z band caches<br/>fb_A/fb_B/z_A/z_B"]:::mem
    sdramctrl["Sdram_Control<br/>board SDR SDRAM conduit"]:::fpga
    linebuf["VGA scanout line buffers x3"]:::mem
    vga["VGA RGB/HS/VS/blank outputs"]:::out
    board_sdram["Board SDR SDRAM pins"]:::out
  end
end
game --> input
game --> world
game --> renderer
world -->|"ChunkMesh faces"| renderer
renderer -->|"quad_desc / quad_desc_uv"| transport
transport -->|"ioctl CLEAR/BEGIN/END/FLIP, palette/fog"| driver
transport -->|"write descriptor words"| driver
driver -->|"MMIO CSR/FIFO writes"| lw
lw --> gpu
hps0 --> lw
hps0 --> h2f
h2f --> fpga_sdram
pll --> gpu
pll --> fpga_sdram
gpu --> fifo
gpu --> caches
gpu --> sdramctrl
sdramctrl --> board_sdram
sdramctrl --> linebuf
linebuf --> vga
""" + class_defs()


def hps_fpga_ownership() -> str:
    return mermaid_header() + """flowchart LR
subgraph HPS["HPS-owned state and work"]
  world["VoxelWorld<br/>chunks, blocks, lighting, redstone, falling blocks"]:::hps
  mesh["ChunkMesh snapshots<br/>atomic live_mesh"]:::hps
  renderbuf["RenderContext submit_buffer<br/>quad_desc stream"]:::hps
  bins["gpu_transport per-band bins<br/>g_bins_pool[2][8]"]:::hps
  kdrv["Kernel driver bounce buffer<br/>VOXEL_BOUNCE_WORDS=2048"]:::hps
end
subgraph CROSS["Boundary crossings"]
  ioctls["ioctl ABI<br/>CLEAR, BEGIN_BAND, END_BAND, FLIP, SET_*"]:::bus
  writes["write() descriptor stream<br/>32-bit FIFO words"]:::bus
  regs["Avalon-MM CSR window<br/>0x0000..0x006C + FIFO 0x1000..0x1FFF"]:::reg
end
subgraph FPGA["FPGA-owned state and work"]
  csr["voxel_gpu CSRs<br/>control/status/palette/fog/band/extmem/perf"]:::reg
  fifo["fifo_mem[1024] descriptor FIFO"]:::mem
  pipe["voxel_gpu raster pipeline<br/>pipe0..commit"]:::fpga
  tex["texture ROM + reciprocal LUT + palettes"]:::mem
  cache["color/Z ping-pong band caches"]:::mem
  ext["external SDRAM front/back color frames"]:::mem
  scan["VGA counters + scanout line buffers"]:::fpga
end
world --> mesh --> renderbuf --> bins --> writes --> fifo --> pipe
bins --> ioctls --> csr
kdrv --> regs --> csr
pipe --> tex
pipe --> cache --> ext --> scan
""" + class_defs()


def hps_software_architecture() -> str:
    return mermaid_header() + """flowchart TB
subgraph GAME["sw/game.c"]
  main["main()"]:::hps
  loop["while (!inp.quit)<br/>update/input/physics/stream/render"]:::hps
  worldtick["world_water_tick(), world_update_redstone(), world_update_falling_blocks()"]:::hps
  stream["world_stream_around(), gen_worker_drain_pending(), mesh_worker_drain_dirty()"]:::hps
end
subgraph WORLD["sw/world.* + workers"]
  world["VoxelWorld"]:::hps
  chunk["Chunk blocks/light/redstone arrays"]:::hps
  mesh["ChunkMesh faces published through live_mesh"]:::hps
end
subgraph RENDER["sw/renderer.c"]
  rinit["renderer_init()<br/>open transport, upload palettes/fog"]:::hps
  begin["renderer_begin_frame()<br/>gpu_transport_begin_descriptors()"]:::hps
  draw["renderer_draw_sky/world/UI"]:::hps
  stage["stage_prepared_quad()<br/>build quad_desc + optional quad_desc_uv"]:::hps
  endf["renderer_end_frame()<br/>clear, flush GPU state, submit, flip"]:::hps
end
subgraph TRANSPORT["sw/gpu_transport.c"]
  open["gpu_transport_open()<br/>/dev/voxel_gpu or socket"]:::hps
  bin["gpu_transport_bin_descriptor()<br/>clip/bin to 8 bands"]:::hps
  submit["submit_hw_prebinned()<br/>BEGIN_BAND/write/END_BAND"]:::hps
  flip["gpu_transport_flip()<br/>FLIP_ASYNC/WAIT_FLIP"]:::hps
end
subgraph DRIVER["sw/voxel_gpu.c"]
  ioctl["voxel_ioctl()<br/>control/status/palette/fog/extmem/band/perf"]:::hps
  write["voxel_write()<br/>iowrite32_rep FIFO_WINDOW"]:::hps
end
main --> loop
loop --> worldtick
loop --> stream
stream --> world --> chunk --> mesh
loop --> begin --> draw --> stage --> bin
loop --> endf --> submit --> ioctl
submit --> write
endf --> flip --> ioctl
rinit --> open
""" + class_defs()


def hps_to_fpga_dataflow() -> str:
    return mermaid_header() + """flowchart LR
world["VoxelWorld / ChunkMesh<br/>sw/world.h, sw/world.c"]:::hps
render["renderer.c<br/>project faces, build quad_desc"]:::hps
stage["stage_prepared_quad()<br/>64B base + 36B UV extension if textured"]:::hps
bin["gpu_transport.c<br/>bin_one_descriptor() by 60-line band"]:::hps
begin["VOXEL_IOC_BEGIN_BAND<br/>BAND_INDEX + BAND_WINDOW + BEGIN"]:::bus
stream["write() descriptor bytes<br/>kernel voxel_write()"]:::bus
endband["VOXEL_IOC_END_BAND<br/>BAND_CTRL.FLUSH"]:::bus
flip["VOXEL_IOC_FLIP_ASYNC/WAIT_FLIP<br/>CONTROL.FLP + STATUS.VSY"]:::bus
csr["voxel_gpu CSR writes"]:::reg
fifo["FIFO window 0x1000..0x1FFF<br/>fifo_mem[1024]"]:::mem
fetch["ST_FETCH desc_words[]"]:::fpga
setup["ST_SETUP raster_setup"]:::math
draw["ST_DRAW pipeline"]:::fpga
cache["640x60 band color/Z cache"]:::mem
sdram["inactive SDRAM frame"]:::mem
scan["VGA scanout active SDRAM frame"]:::out
world --> render --> stage --> bin
bin --> begin --> csr
bin --> stream --> fifo --> fetch --> setup --> draw --> cache
bin --> endband --> csr --> cache --> sdram
bin --> flip --> csr --> scan
sdram --> scan
""" + class_defs()


def register_interface_flow() -> str:
    return mermaid_header() + """flowchart LR
subgraph USER["Userspace"]
  renderer["renderer_flush_gpu_state()<br/>palette, sky palette, fog"]:::hps
  transport["gpu_transport_submit_descriptors()<br/>band submit + flip"]:::hps
end
subgraph KERNEL["Kernel sw/voxel_gpu.c"]
  ioctl["voxel_ioctl() dispatch"]:::hps
  low["voxel_rd()/voxel_wr()<br/>ioread32/iowrite32"]:::hps
  write["voxel_write()<br/>iowrite32_rep(base + VOXEL_FIFO_BASE)"]:::hps
  poll["voxel_poll_status()<br/>STATUS.BSY/FEM/VSY"]:::hps
end
subgraph RTL["voxel_gpu Avalon slave"]
  control["CONTROL<br/>ctrl_en, ctrl_flp_pending, clear_pending"]:::reg
  status["STATUS<br/>engine_busy, fifo flags, vsy_latch"]:::reg
  palette["PALETTE/SKY/Fog registers"]:::reg
  extmem["EXTMEM_* registers + dma_status"]:::reg
  band["BAND_INDEX/WINDOW/CTRL<br/>begin/flush pending"]:::reg
  perf["PERF_* read counters"]:::reg
  fifo["FIFO_WINDOW<br/>fifo_mem + fifo_count"]:::mem
end
renderer --> ioctl
transport --> ioctl
transport --> write
ioctl --> low
low --> control
low --> palette
low --> extmem
low --> band
low --> status
low --> perf
write --> fifo
poll --> status
control -->|"FLP/CLR pulses"| status
band -->|"BEGIN/FLUSH"| fifo
""" + class_defs()


def soc_system_context() -> str:
    return mermaid_header() + """flowchart LR
top["soc_system_top.sv<br/>board pins"]:::out
soc["soc_system<br/>Platform Designer generated system"]:::bus
hps["hps_0<br/>HPS IO + DDR3 conduit"]:::bus
clk["clk_0 50 MHz<br/>sdram_clocks.sys_clk"]:::ctrl
gpu["voxel_gpu_0<br/>kind=voxel_gpu"]:::fpga
fpga_sdram["fpga_sdram<br/>altera_avalon_new_sdram_controller"]:::mem
vga["exported vga conduit"]:::out
dram["exported voxel_sdram conduit"]:::out
ddr["HPS DDR3 pins"]:::out
top --> soc
soc --> hps --> ddr
hps -->|"h2f_lw_axi_master base 0x0000"| gpu
hps -->|"h2f_axi_master base 0x0000"| fpga_sdram
clk --> gpu
clk --> fpga_sdram
gpu --> vga
gpu --> dram
""" + class_defs()


def voxel_gpu_module_hierarchy() -> str:
    return mermaid_header() + """flowchart TB
gpu["voxel_gpu<br/>Avalon slave, FSM, pipeline, SDRAM/VGA arbitration"]:::fpga
front["CSR/FIFO front door<br/>ADDR_* map, fifo_mem"]:::reg
fsm["engine_state_t FSM<br/>IDLE/CLEAR/FETCH/SETUP/DRAW/FLUSH/CACHE_INIT"]:::ctrl
setup["voxel_raster_setup<br/>edge/depth/UV initial values"]:::math
drawstep["voxel_draw_step<br/>2-pixel edge/depth/UV stepping"]:::math
recip["voxel_iw_normalize x2<br/>voxel_recip_interpolate x2<br/>voxel_w_denormalize x2"]:::math
fog["voxel_fog_blend x2"]:::math
perf["voxel_perf_counters<br/>frame activity counters"]:::reg
ram["voxel_banked_sdp_ram x4<br/>fb_back_ram_A/B, z_ram_A/B"]:::mem
texture["voxel_texture_rom<br/>dual read texture atlas"]:::mem
vga["voxel_vga_counters"]:::fpga
sdram["Sdram_Control<br/>WR/RD FIFO interface to board SDRAM"]:::fpga
gpu --> front
gpu --> fsm
gpu --> setup
gpu --> drawstep
gpu --> recip
gpu --> fog
gpu --> perf
gpu --> ram
gpu --> texture
gpu --> vga
gpu --> sdram
ram -->|"contains"| bank["voxel_sdp_ram banks<br/>vendor altsyncram"]:::mem
texture -->|"uses"| altsyncram["altsyncram ROM copies"]:::mem
sdram -->|"uses"| fifos["Sdram_WR_FIFO / Sdram_RD_FIFO"]:::mem
""" + class_defs()


def voxel_gpu_datapath() -> str:
    return mermaid_header() + """flowchart LR
subgraph FRONT["Avalon-MM front door"]
  csr["CSRs<br/>control/status/palette/fog/extmem/band/perf"]:::reg
  fifo["FIFO_WINDOW writes<br/>fifo_mem[1024]"]:::mem
end
subgraph DESC["Descriptor fetch/setup"]
  fetch["ST_FETCH<br/>desc_words[0..24]"]:::fpga
  unpack["desc_* wires<br/>bbox, edges, z, flags, UV planes"]:::fpga
  setup["voxel_raster_setup<br/>MUL edge A*x/B*y, ADD C, setup z/UV/IW"]:::math
end
subgraph DRAW["2-pixel raster walk"]
  step["voxel_draw_step<br/>edge ADD, z/UV/IW ADD, next pair/row"]:::math
  inside["edge comparators<br/>inside lane0/lane1"]:::math
  zrd["color/Z cache read<br/>fb_draw_rd_data, z_draw_rd_data"]:::mem
end
subgraph PIPE["Pixel pipeline"]
  pipe0["pipe0 regs<br/>addr/z/uv/iw/flags"]:::reg
  recip["recip stages<br/>normalize, LUT, interpolate, denormalize"]:::math
  uv["UV multiply<br/>u_over_w*w, v_over_w*w"]:::math
  tex["texture_coord + voxel_texture_rom"]:::mem
  pal["apply_light_bank + palette/sky palette"]:::mem
  fog["radial distance MUL/ADD + voxel_fog_blend"]:::math
  commit["commit_valid/pass<br/>alpha, z-test comparator, color/Z write"]:::reg
end
subgraph CACHE["Band cache and external frame"]
  band["ping-pong color/Z band caches<br/>640x60"]:::mem
  flush["ST_CACHE_INIT + background flush<br/>Z init and SDRAM write bursts"]:::fpga
  sdram["board SDR SDRAM frames<br/>front/back color"]:::mem
end
subgraph OUT["Display"]
  scan["scanout line buffers x3<br/>SDRAM read bursts"]:::mem
  vga["VGA RGB + timing"]:::out
end
csr --> fetch
fifo --> fetch --> unpack --> setup --> step --> inside --> pipe0
zrd --> pipe0
pipe0 --> recip --> uv --> tex --> pal --> fog --> commit --> band
band --> flush --> sdram --> scan --> vga
""" + class_defs()


def voxel_gpu_pipeline() -> str:
    return mermaid_header() + """flowchart LR
subgraph S0["S0 ST_FETCH"]
  s0["desc_words[] registers<br/>FIFO pop, descriptor size 16/25 words"]:::reg
end
subgraph S1["S1 ST_SETUP"]
  s1a["voxel_raster_setup<br/>MUL edge A*x/B*y"]:::math
  s1b["edge_row/cur, z_row/cur,<br/>uw/vw/iw row/cur registers"]:::reg
end
subgraph S2["S2 ST_DRAW issue"]
  s2a["voxel_draw_step<br/>ADD next pair/row"]:::math
  s2b["inside CMP, bbox CMP,<br/>cache address, z clamp"]:::math
  s2c["pipe0 / pipe0_o registers"]:::reg
end
subgraph S3["S3 reciprocal normalize"]
  s3["recip0 registers<br/>iw_msb, iw_norm, cache z/color read alignment"]:::reg
end
subgraph S4["S4 reciprocal LUT"]
  s4["recip1 registers<br/>recip_lut lo/hi, z_ref, dst_rgb565"]:::reg
end
subgraph S5["S5 reciprocal interpolate"]
  s5["recip2 registers<br/>voxel_recip_interpolate"]:::reg
end
subgraph S6["S6 denormalize / UV multiply"]
  s6["pipe1 registers<br/>voxel_w_denormalize + MUL u/v"]:::reg
end
subgraph S7["S7 texture address"]
  s7["tex0 registers<br/>texture_coord, tex_addr"]:::reg
end
subgraph S8["S8 texture ROM latency"]
  s8["pipe2 registers<br/>voxel_texture_rom address"]:::reg
end
subgraph S9["S9 color select"]
  s9["draw_pipe registers<br/>texel or flat color, light bank"]:::reg
end
subgraph S10["S10 palette address"]
  s10["pal_rd registers<br/>palette/sky/fog addresses, z pass"]:::reg
end
subgraph S11["S11 palette data"]
  s11["plr registers<br/>palette RGB + fog RGB"]:::reg
end
subgraph S12["S12 fog setup"]
  s12["fog0 registers<br/>RGB565 + radial distance product"]:::reg
end
subgraph S13["S13 fog blend"]
  s13["fog1 registers<br/>radial_q8_8 + voxel_fog_blend"]:::reg
end
subgraph S14["S14 commit"]
  s14["commit registers<br/>z-test pass, alpha/fog color, color/Z write"]:::reg
end
s0 --> s1a --> s1b --> s2a --> s2b --> s2c --> s3 --> s4 --> s5 --> s6 --> s7 --> s8 --> s9 --> s10 --> s11 --> s12 --> s13 --> s14
""" + class_defs()


def voxel_gpu_control_fsm() -> str:
    return mermaid_header() + """stateDiagram-v2
[*] --> ST_IDLE
ST_IDLE --> ST_CLEAR: clear_pending
ST_CLEAR --> ST_IDLE: clear bookkeeping done
ST_IDLE --> ST_CACHE_INIT: BAND_CTRL.BEGIN and cache available
ST_CACHE_INIT --> ST_IDLE: band window initialized
ST_IDLE --> ST_FETCH: ctrl_en and FIFO has at least base descriptor
ST_FETCH --> ST_SETUP: descriptor complete and not skipped
ST_FETCH --> ST_IDLE: degenerate or redundant sky clear descriptor
ST_SETUP --> ST_DRAW: setup registers loaded
ST_DRAW --> ST_DRAW_FLUSH: descriptor row/bbox complete or cache miss/drain
ST_DRAW_FLUSH --> ST_FETCH: prefetched descriptor ready
ST_DRAW_FLUSH --> ST_IDLE: pipeline drained
note right of ST_IDLE
  Also arbitrates BAND_CTRL.FLUSH background flush,
  CONTROL.FLP copy_complete_pending, prefetch handoff,
  and ctrl_clear_write abort behavior.
end note
"""


def memory_and_buffer_ownership() -> str:
    return mermaid_header() + """flowchart LR
subgraph HPS["HPS memory"]
  chunks["Chunk arrays<br/>blocks, sky_light, block_light, water_level, redstone_data"]:::hps
  meshes["ChunkMesh immutable face snapshots"]:::hps
  submit["RenderContext submit_buffer"]:::hps
  bins["g_bins_pool[2][VOXEL_BAND_COUNT]"]:::hps
  bounce["kernel voxel_write bounce buffer"]:::hps
end
subgraph FPGA["FPGA internal memory"]
  csr["CSRs + control/status flops"]:::reg
  fifo["fifo_mem[1024]"]:::mem
  desc["desc_words / prefetch_words"]:::reg
  pal["palette[256] + sky_palette[24]"]:::mem
  recip["recip_lut[1025-ish indexed entries]"]:::mem
  tex["voxel_texture_rom atlas<br/>textures.mif"]:::mem
  cache["fb_A/fb_B/z_A/z_B band caches"]:::mem
  line["scan_linebuf0/1/2"]:::mem
end
subgraph EXTERNAL["External or board-visible"]
  sdram["Board SDR SDRAM<br/>front/back color frames"]:::mem
  vga["VGA DAC pins"]:::out
end
chunks -->|"world.c writes/reads"| meshes
meshes -->|"renderer reads"| submit --> bins --> bounce --> fifo --> desc
csr --> pal
desc --> cache
pal --> cache
recip --> cache
tex --> cache
cache -->|"flush writes"| sdram
sdram -->|"scanout reads"| line --> vga
""" + class_defs()


def game_to_pixels_flow() -> str:
    return mermaid_header() + """flowchart LR
game["game.c frame loop<br/>input, physics, world sim"]:::hps
world["VoxelWorld<br/>streamed chunks + live ChunkMesh"]:::hps
draw["renderer_draw_sky/world/UI"]:::hps
quad["stage_prepared_quad()<br/>screen bbox, edge coeffs, z gradients, UV planes"]:::hps
band["gpu_transport<br/>clip to 8 x 60-line bands"]:::hps
driver["/dev/voxel_gpu<br/>ioctl + write FIFO"]:::hps
csr["voxel_gpu CSR/FIFO front door"]:::reg
setup["ST_FETCH / ST_SETUP"]:::fpga
pipe["ST_DRAW pixel pipeline<br/>edge test, reciprocal, texture, palette, fog, z-test"]:::fpga
cache["resident color/Z band cache"]:::mem
flush["END_BAND flush to inactive SDRAM frame"]:::fpga
flip["FLIP on vsync<br/>display_sel/copy_target_sel"]:::ctrl
scan["SDRAM scanout line buffers"]:::mem
vga["VGA pixels"]:::out
game --> world --> draw --> quad --> band --> driver --> csr --> setup --> pipe --> cache --> flush --> flip --> scan --> vga
""" + class_defs()


def c_call_graph() -> str:
    return mermaid_header() + """flowchart TB
main["game.c main()"]:::hps
rinit["renderer_init()"]:::hps
gopen["gpu_transport_open()"]:::hps
begin["renderer_begin_frame()"]:::hps
drawsky["renderer_draw_sky()"]:::hps
drawworld["renderer_draw_world()"]:::hps
stage["stage_prepared_quad()"]:::hps
bin["gpu_transport_bin_descriptor()"]:::hps
endf["renderer_end_frame()"]:::hps
clear["gpu_transport_clear()"]:::hps
flush["renderer_flush_gpu_state()"]:::hps
submit["gpu_transport_submit_descriptors()"]:::hps
prebin["submit_hw_prebinned()"]:::hps
band["submit_hw_band_flat()"]:::hps
flip["gpu_transport_flip()"]:::hps
drv_ioctl["sw/voxel_gpu.c voxel_ioctl()"]:::hps
drv_write["sw/voxel_gpu.c voxel_write()"]:::hps
main --> rinit --> gopen
main --> begin --> bin
main --> drawsky --> stage --> bin
main --> drawworld --> stage
main --> endf
endf --> clear --> drv_ioctl
endf --> flush --> drv_ioctl
endf --> submit --> prebin --> band
band --> drv_ioctl
band --> drv_write
endf --> flip --> drv_ioctl
""" + class_defs()


DIAGRAM_GENERATORS = {
    "full_system_architecture.mmd": full_system_architecture,
    "hps_fpga_ownership.mmd": hps_fpga_ownership,
    "hps_software_architecture.mmd": hps_software_architecture,
    "hps_to_fpga_dataflow.mmd": hps_to_fpga_dataflow,
    "register_interface_flow.mmd": register_interface_flow,
    "soc_system_context.mmd": soc_system_context,
    "voxel_gpu_module_hierarchy.mmd": voxel_gpu_module_hierarchy,
    "voxel_gpu_datapath.mmd": voxel_gpu_datapath,
    "voxel_gpu_pipeline.mmd": voxel_gpu_pipeline,
    "voxel_gpu_control_fsm.mmd": voxel_gpu_control_fsm,
    "memory_and_buffer_ownership.mmd": memory_and_buffer_ownership,
    "game_to_pixels_flow.mmd": game_to_pixels_flow,
    "c_call_graph.mmd": c_call_graph,
}


TRACEABILITY = {
    "full_system_architecture.mmd": [
        "hw/soc_system.qsys",
        "hw/soc_system_top.sv",
        "hw/voxel_gpu/rtl/voxel_gpu.sv",
        "sw/game.c",
        "sw/renderer.c",
        "sw/gpu_transport.c",
        "sw/voxel_gpu.c",
    ],
    "hps_fpga_ownership.mmd": [
        "sw/world.h",
        "sw/renderer.c",
        "sw/gpu_transport.c",
        "sw/voxel_gpu.c",
        "hw/voxel_gpu/rtl/voxel_gpu.sv",
    ],
    "hps_software_architecture.mmd": [
        "sw/game.c",
        "sw/world.h",
        "sw/renderer.c",
        "sw/gpu_transport.c",
        "sw/voxel_gpu.c",
    ],
    "hps_game_loop_flow.mmd": [
        "sw/game.c::main",
        "sw/input.h",
        "sw/player_physics.c",
        "sw/world.c",
        "sw/renderer.c",
        "sw/gen_worker.h",
        "sw/mesh_worker.h",
    ],
    "hps_world_streaming_mesh_flow.mmd": [
        "sw/game.c::main",
        "sw/world.h",
        "sw/world.c::world_stream_around",
        "sw/world.c::world_run_mesh_job",
        "sw/gen_worker.h",
        "sw/mesh_worker.h",
        "sw/renderer.c::renderer_draw_world",
    ],
    "hps_interaction_logic_flow.mmd": [
        "sw/game.c::trace_target_block",
        "sw/game.c::break_block_target",
        "sw/game.c::try_place_targeted_block",
        "sw/game.c::try_use_held_bucket",
        "sw/game.c::try_toggle_targeted_door",
        "sw/game.c::try_press_targeted_button",
        "sw/game.c::try_toggle_targeted_lever",
        "sw/world.c::world_set_block_locked",
        "sw/inventory.h",
        "sw/game_items.h",
    ],
    "hps_game_logic_breakdown.md": [
        "sw/game.c",
        "sw/game_home.c",
        "sw/input.h",
        "sw/player_physics.c",
        "sw/world.h",
        "sw/world.c",
        "sw/world_gen.c",
        "sw/gen_worker.h",
        "sw/mesh_worker.h",
        "sw/block_types.h",
        "sw/inventory.h",
        "sw/game_items.h",
        "sw/command_parser.h",
        "sw/renderer.c",
        "sw/gpu_transport.c",
        "sw/voxel_gpu.h",
    ],
    "hps_to_fpga_dataflow.mmd": [
        "sw/renderer.c",
        "sw/gpu_transport.c",
        "sw/voxel_gpu.c",
        "sw/voxel_gpu.h",
        "hw/voxel_gpu/rtl/voxel_gpu.sv",
    ],
    "register_interface_flow.mmd": ["sw/voxel_gpu.h", "sw/voxel_gpu.c", "hw/voxel_gpu/rtl/voxel_gpu.sv"],
    "register_map.md": ["sw/voxel_gpu.h", "sw/voxel_gpu.c", "hw/voxel_gpu/rtl/voxel_gpu.sv"],
    "soc_system_context.mmd": ["hw/soc_system.qsys", "hw/soc_system_top.sv", "hw/voxel_gpu_hw.tcl"],
    "voxel_gpu_module_hierarchy.mmd": ["hw/voxel_gpu/rtl/*.sv", "hw/voxel_gpu_hw.tcl"],
    "voxel_gpu_datapath.mmd": ["hw/voxel_gpu/rtl/voxel_gpu.sv", "hw/voxel_gpu/rtl/voxel_math_utils.sv"],
    "voxel_gpu_pipeline.mmd": ["hw/voxel_gpu/rtl/voxel_gpu.sv", "hw/voxel_gpu/rtl/voxel_math_utils.sv"],
    "voxel_gpu_control_fsm.mmd": ["hw/voxel_gpu/rtl/voxel_gpu.sv"],
    "memory_and_buffer_ownership.mmd": ["sw/world.h", "sw/renderer.c", "sw/gpu_transport.c", "hw/voxel_gpu/rtl/voxel_gpu.sv"],
    "game_to_pixels_flow.mmd": ["sw/game.c", "sw/renderer.c", "sw/gpu_transport.c", "sw/voxel_gpu.c", "hw/voxel_gpu/rtl/voxel_gpu.sv"],
    "raster_setup_operator_datapath.svg": ["hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_raster_setup", "hw/voxel_gpu/rtl/voxel_gpu.sv::ST_SETUP"],
    "raster_draw_step_operator_datapath.svg": ["hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_draw_step", "hw/voxel_gpu/rtl/voxel_gpu.sv::ST_DRAW"],
    "texture_pipeline_operator_datapath.svg": ["hw/voxel_gpu/rtl/voxel_math_utils.sv", "hw/voxel_gpu/rtl/voxel_gpu.sv::pipe0..commit", "hw/voxel_gpu/rtl/voxel_texture_rom.sv"],
    "memory_access_operator_datapath.svg": ["hw/voxel_gpu/rtl/voxel_gpu.sv::cache muxing and commit fanout", "hw/voxel_gpu/rtl/voxel_sdp_ram.sv", "hw/voxel_gpu/rtl/voxel_banked_sdp_ram.sv", "hw/sdram_local_test/Sdram_Control.v"],
    "operator_level_datapaths.md": ["scripts/gen_operator_diagrams.py", "hw/voxel_gpu/rtl/voxel_math_utils.sv", "hw/voxel_gpu/rtl/voxel_gpu.sv", "hw/voxel_gpu/rtl/voxel_perf_counters.sv", "hw/voxel_gpu/rtl/voxel_sdp_ram.sv", "hw/voxel_gpu/rtl/voxel_banked_sdp_ram.sv"],
    "voxel_gpu_timing.wave.json": ["tb/voxel_gpu_tb.sv", "build/diagrams/voxel_gpu.vcd when present"],
    "voxel_gpu_timing.svg": ["docs/diagrams/voxel_gpu_timing.wave.json", "scripts/render_timing_diagram.py"],
    "c_call_graph.mmd": ["sw/game.c", "sw/renderer.c", "sw/gpu_transport.c", "sw/voxel_gpu.c"],
}


def generate_common_diagrams(selected: Iterable[str] | None = None) -> list[Path]:
    ensure_dirs()
    names = list(selected) if selected is not None else sorted(DIAGRAM_GENERATORS)
    outputs: list[Path] = []
    for name in names:
        path = DIAGRAMS / name
        write(path, DIAGRAM_GENERATORS[name]())
        outputs.append(path)
    write(DIAGRAMS / "source_traceability.md", source_traceability_md())
    return outputs


def register_map_md() -> str:
    lines = [
        "# Register Map",
        "",
        "Source of truth: `sw/voxel_gpu.h` register macros and comments, `sw/voxel_gpu.c` ioctl/write paths, and `hw/voxel_gpu/rtl/voxel_gpu.sv` `ADDR_*` decode/readback cases.",
        "",
        "| Byte offset | Register | Dir | RTL address | Bit fields | Reset/default | C usage | Hardware signal(s) | Notes |",
        "|---:|---|---|---|---|---|---|---|---|",
    ]
    for row in register_rows():
        lines.append(
            f"| 0x{row.offset:04X} | `{row.name}` | {row.direction} | `{row.rtl_addr}` | {row.bit_fields} | {row.reset} | {row.c_usage} | `{row.hw_signal}` | {row.notes} |"
        )
    lines.extend(
        [
            "",
            "Uncertainty notes:",
            "",
            "- `EXTMEM_STAT` is readable in RTL as `extmem_dma_status` and partially decoded by diagnostics in `sw/gpu_transport.c`; not every bit has a named public macro in `sw/voxel_gpu.h`.",
            "- `CONTROL[2]` (`IEN`) is stored as `ctrl_ien`, but no interrupt output is visible on the `voxel_gpu` module port list.",
            "- Timing/resource/Fmax values require Quartus reports after a full build; none are extracted here.",
        ]
    )
    return "\n".join(lines)


def hardware_software_interface_md() -> str:
    return f"""# Hardware/Software Interface

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
- {yosys_status()}
- {simulation_status()}
- Quartus timing/resource/utilization values require Quartus reports after a full build.
"""


def diagram_index_md() -> str:
    descriptions = {
        "full_system_architecture.mmd": "Where the HPS software, Platform Designer system, voxel_gpu, SDRAM, and VGA output fit.",
        "hps_fpga_ownership.mmd": "Which side owns world/chunk data, descriptor buffers, CSRs, FIFOs, caches, and output memories.",
        "hps_software_architecture.mmd": "How game.c, renderer.c, gpu_transport.c, world.c, and the kernel driver cooperate.",
        "hps_game_loop_flow.mmd": "The source-level HPS game loop order from startup through simulation, interactions, rendering, and shutdown.",
        "hps_world_streaming_mesh_flow.mmd": "How player position drives chunk streaming, async generation, mesh publishing, renderer reads, and retired mesh cleanup.",
        "hps_interaction_logic_flow.mmd": "How normal gameplay input becomes targeting, breaking, placement, interactive block actions, world mutation, and mesh updates.",
        "hps_game_logic_breakdown.md": "The comprehensive HPS-side game logic breakdown with source-file/function traceability.",
        "hps_to_fpga_dataflow.mmd": "How world/chunk data becomes descriptors and crosses into voxel_gpu.",
        "register_interface_flow.mmd": "Which C APIs/macros drive which RTL registers/signals.",
        "register_map.md": "The extracted hardware/software register map.",
        "soc_system_context.mmd": "The Platform Designer placement of voxel_gpu and HPS bridges.",
        "voxel_gpu_module_hierarchy.mmd": "The major RTL modules instantiated by voxel_gpu.",
        "voxel_gpu_datapath.mmd": "The readable datapath through fetch, setup, draw, cache, SDRAM, and VGA.",
        "voxel_gpu_pipeline.mmd": "The actual named pipeline register progression from pipe0 through commit.",
        "voxel_gpu_control_fsm.mmd": "The engine_state_t state flow and key transition conditions.",
        "memory_and_buffer_ownership.mmd": "Who writes and reads each important memory/buffer.",
        "game_to_pixels_flow.mmd": "End-to-end game/world-to-VGA explanation.",
        "raster_setup_operator_datapath.svg": "Whiteboard-style operator diagram for edge-equation setup and start-value correction.",
        "raster_draw_step_operator_datapath.svg": "Whiteboard-style operator diagram for two-pixel raster stepping, inside tests, and pair/row increments.",
        "texture_pipeline_operator_datapath.svg": "Whiteboard-style operator diagram for reciprocal, UV multiply, texture, palette, fog, and z-test operators.",
        "memory_access_operator_datapath.svg": "Whiteboard-style operator diagram for active cache reads, commit writes, bank steering, SDRAM copy/load, and scanout.",
        "operator_level_datapaths.md": "Readable page containing the rendered operator-level SVG diagrams.",
        "voxel_gpu_timing.wave.json": "WaveDrom skeleton or VCD-derived timing if a real VCD is present.",
        "voxel_gpu_timing.svg": "Static rendered view of the WaveDrom timing asset.",
        "c_call_graph.mmd": "Main C call path from game loop to hardware access functions.",
        "source_traceability.md": "Source files used for each diagram.",
    }
    lines = [
        "# Diagram Index",
        "",
        "Readable rendered gallery: `docs/diagrams/index.html`.",
        "Single Markdown bundle: `docs/diagrams/all_diagrams.md`.",
        "",
    ]
    for name in sorted(descriptions):
        sources = ", ".join(f"`{s}`" for s in TRACEABILITY.get(name, []))
        lines.append(f"## `{name}`")
        lines.append("")
        lines.append(f"- Answers: {descriptions[name]}")
        lines.append(f"- Based on: {sources}")
        if name in {"voxel_gpu_timing.wave.json", "voxel_gpu_timing.svg"}:
            lines.append(f"- Limitation: {simulation_status()}")
        elif name == "hps_game_logic_breakdown.md":
            lines.append("- Limitation: source-level HPS game-logic document; runtime performance and observed gameplay behavior require logs or hardware/simulation evidence.")
        elif name.endswith("_operator_datapath.svg") or name == "operator_level_datapaths.md":
            lines.append("- Limitation: source-grounded schematic, not a post-synthesis netlist or Quartus resource/timing report.")
        elif name == "register_map.md":
            lines.append("- Limitation: bit fields are limited to fields named in source comments/macros/RTL readback.")
        elif name == "soc_system_context.mmd":
            lines.append("- Limitation: generated Platform Designer HDL is not present, so this uses `.qsys` and top-level exported conduits.")
        else:
            lines.append("- Limitation: readable source-level diagram, not a full gate/netlist rendering.")
        lines.append("")
    return "\n".join(lines)


def source_traceability_md() -> str:
    lines = ["# Source Traceability", ""]
    for name in sorted(TRACEABILITY):
        lines.append(f"## `{name}`")
        for source in TRACEABILITY[name]:
            lines.append(f"- `{source}`")
        lines.append("")
    lines.extend(
        [
            "Extraction notes:",
            "",
            "- RTL register and FSM facts come from `hw/voxel_gpu/rtl/voxel_gpu.sv`.",
            "- Software interface facts come from `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, `sw/gpu_transport.c`, and `sw/renderer.c`.",
            "- Platform Designer facts come from `hw/soc_system.qsys`, `hw/soc_system_top.sv`, and `hw/voxel_gpu_hw.tcl`.",
            "- Timing behavior is not invented; the WaveDrom file is marked as a skeleton unless a VCD exists.",
        ]
    )
    return "\n".join(lines)


def final_breakdown_md() -> str:
    modules = extract_modules()
    insts = extract_instances()
    states = extract_states()
    inst_lines = "\n".join(f"- `{inst}`: `{mod}`" for mod, inst in insts)
    state_text = ", ".join(f"`{s}`" for s in states)
    return f"""# Final Technical Breakdown

## Project Overview

This is a Minecraft-style voxel game split across HPS-side C software, a Linux kernel device driver, Platform Designer system integration, and custom SystemVerilog RTL. The main system context is `soc_system` in `hw/soc_system.qsys`; the board wrapper is `hw/soc_system_top.sv`; the main custom FPGA datapath is `hw/voxel_gpu/rtl/voxel_gpu.sv`.

## Evidence From Source

- `hw/soc_system.qsys` instantiates `voxel_gpu_0` of kind `voxel_gpu`.
- `hps_0.h2f_lw_axi_master` connects to `voxel_gpu_0.avalon_slave_0` at base `0x0000`.
- `hps_0.h2f_axi_master` connects to `fpga_sdram.s1` at base `0x0000`.
- `hw/voxel_gpu_hw.tcl` declares the `voxel_gpu` Avalon slave, VGA conduit, and `voxel_sdram` conduit.
- `sw/voxel_gpu.c` exposes `/dev/voxel_gpu`, maps the FPGA register resource, handles ioctls, and streams FIFO words.
- `sw/renderer.c` builds `struct quad_desc` / `struct quad_desc_uv` descriptors and submits them through `sw/gpu_transport.c`.

## HPS/Software Architecture

`sw/game.c::main()` owns the frame loop. It updates input, player physics, environment simulation, redstone, falling blocks, chunk streaming, lighting, async chunk generation, and mesh draining. It then calls `renderer_begin_frame()`, draw functions for sky/world/entities/UI, and `renderer_end_frame()`.

`VoxelWorld` in `sw/world.h` owns chunk arrays, block IDs, lighting, fluid/redstone metadata, live `ChunkMesh` snapshots, streaming state, and worker synchronization. The renderer reads immutable `ChunkMesh` snapshots and emits packed descriptors into `RenderContext::submit_buffer`.

`sw/gpu_transport.c` owns hardware submission policy: per-band descriptor bins, optional band reuse, `BEGIN_BAND` / descriptor `write()` / `END_BAND`, and flip handling. The kernel driver does not parse descriptors.

## FPGA/Hardware Architecture

`voxel_gpu` contains the CSR/FIFO front door, descriptor fetch, raster setup, two-pixel draw walk, reciprocal/texture/palette/fog pipeline, color/Z commit, ping-pong band caches, SDRAM write/read arbitration, line buffers, and VGA output.

Major instantiated modules:

{inst_lines}

## soc_system Overview

`soc_system_top.sv` instantiates `soc_system soc_system0` and exports HPS DDR3 pins, VGA pins, and the voxel SDRAM pins. The `.qsys` file shows `sdram_clocks.sys_clk` feeding the FPGA SDRAM controller, `voxel_gpu_0.clock`, and HPS bridge clocks. The generated `soc_system` HDL is not present in this repo snapshot, so system internals beyond `.qsys` are documented from the Platform Designer file rather than guessed.

## End-To-End Game-To-Pixels Dataflow

World/chunk state flows from `VoxelWorld` and `ChunkMesh` into `renderer_draw_world()`. `stage_prepared_quad()` computes screen-space bounds, edge coefficients, depth gradients, and optional perspective-correct UV planes. `gpu_transport_bin_descriptor()` clips descriptors into eight 60-line bands. The kernel driver writes CSR commands and descriptor words into the `voxel_gpu` Avalon window. `voxel_gpu` fetches descriptors, rasterizes pixels into a resident color/Z band cache, flushes dirty rows to the inactive SDRAM frame, and scanout reads the active SDRAM frame into VGA line buffers.

## HPS-To-FPGA Interface

The register map is in `docs/diagrams/register_map.md`. Control/status facts are extracted from `sw/voxel_gpu.h`, `sw/voxel_gpu.c`, and `hw/voxel_gpu/rtl/voxel_gpu.sv`. The FIFO window is `0x1000..0x1FFF` in C byte offsets and `ADDR_FIFO_LO=13'h400` through `ADDR_FIFO_HI=13'h800` in RTL word addresses.

The driver synchronizes by polling `STATUS.FEM`, `STATUS.BSY`, and `STATUS.VSY`. No interrupt output is visible in the `voxel_gpu` module ports; `CONTROL[2]` is stored as `ctrl_ien`, but no source-visible interrupt path is connected.

## voxel_gpu Datapath and Pipeline

The engine FSM states are: {state_text}.

The draw pipeline uses named register groups visible in RTL: `pipe0`, `recip0`, `recip1`, `recip2`, `pipe1`, `tex0`, `pipe2`, `draw_pipe`, `pal_rd`, `plr`, `fog0`, `fog1`, and `commit`, with duplicate `_o` groups for the odd lane. Operators visible in the source include edge/depth/UV adders, edge/bbox/z comparators, reciprocal normalization shifts, reciprocal interpolation multiply/subtract, UV multiply, texture coordinate selection, palette/light-bank selection, radial fog multiply/add, alpha/fog blending, and color/Z RAM writes.

Important memories and buffers are `fifo_mem`, `palette`, `sky_palette`, `recip_lut`, the dual-read `voxel_texture_rom`, four banked color/Z band RAMs, three scanout line buffers, and external board SDRAM frames.

## Timing and Latency Evidence

The RTL explicitly pipelines palette read and texture ROM latency with named stages, and `DRAW_FLUSH_CYCLES=14` drains valid stages between fetch and color commit. {simulation_status()} The current VCD comes from `tb/voxel_gpu_tb.sv`; `tb/sim_stubs.sv` supplies simulation-only models for the Quartus PLL/RAM/FIFO primitives. No cycle-accurate timing is claimed beyond captured simulator signals. Quartus Fmax/slack/resource/utilization values require Quartus report after full build.

## Netlist Evidence

{yosys_status()} If Yosys is installed, run `scripts/gen_netlist_json.sh` or `make diagrams` to produce `build/diagrams/voxel_gpu.json`.

## Testing and Validation Evidence In Repo

Software tests exist under `sw/tests/`, including renderer, world, inventory, command parser, player physics, FPGA SDRAM offset/band tests. A Python virtual hardware monitor exists under `virtual_hw/`, including raster tests. No dedicated RTL testbench directory existed before this workflow; `tb/voxel_gpu_tb.sv` is a minimal simulation-only harness.

## Known Limitations and Current Behavior

- No Quartus timing/resource reports are available in this repo snapshot, so no Fmax, slack, ALM/LUT/DSP/RAM/register counts, or timing-closure claims are made.
- Generated Platform Designer HDL under `hw/soc_system/synthesis/` is not present, so `soc_system` diagrams use `.qsys` and wrapper evidence.
- `voxel_gpu_timing.wave.json` and `voxel_gpu_timing.svg` are VCD-derived when `build/diagrams/voxel_gpu.vcd` exists; otherwise the WaveDrom file is a marked skeleton.
- The VCD uses simulation-only primitive stubs and minimal MMIO/FIFO stimulus, so it is not a Quartus timing report or a full rendered-scene validation.
- The Yosys netlist flow depends on Yosys and may also need vendor primitive handling for `altsyncram`/FIFO/PLL components; the script emits an actionable log.
"""


def file_listings_md() -> str:
    groups = [
        ("Hardware RTL and Platform Designer files", ["hw/**/*.sv", "hw/**/*.svh", "hw/**/*.v", "hw/*.qsys", "hw/*.qsf", "hw/*.qpf", "hw/*.sdc", "hw/*.tcl", "hw/*.xml"]),
        ("Software C/header/build files", ["sw/**/*.c", "sw/**/*.h", "sw/Makefile"]),
        ("Virtual hardware and tests", ["virtual_hw/**/*.py", "virtual_hw/**/*.toml", "virtual_hw/**/*.lock"]),
        ("Scripts", ["scripts/*"]),
        ("Documentation", ["README.md", "PROJECT_NOTES.md", "docs/**/*.md"]),
        ("Generated diagrams", ["docs/diagrams/*", "build/diagrams/*"]),
    ]
    lines = ["# File Listings", "", "This listing is for project understanding only.", ""]
    for title, patterns in groups:
        lines.append(f"## {title}")
        files = all_source_files(patterns)
        if not files:
            lines.append("- none found")
        for path in files:
            lines.append(f"- `{rel(path)}`")
        lines.append("")
    return "\n".join(lines)


def generate_breakdown_docs() -> list[Path]:
    ensure_dirs()
    outputs = {
        DIAGRAMS / "register_map.md": register_map_md(),
        DIAGRAMS / "hardware_software_interface.md": hardware_software_interface_md(),
        DIAGRAMS / "diagram_index.md": diagram_index_md(),
        DIAGRAMS / "source_traceability.md": source_traceability_md(),
        DOCS / "final_breakdown.md": final_breakdown_md(),
        DOCS / "file_listings.md": file_listings_md(),
    }
    for path, text in outputs.items():
        write(path, text)
    return sorted(outputs)


def write_source_model_json() -> Path:
    ensure_dirs()
    data = {
        "modules": extract_modules(),
        "voxel_gpu_instances": extract_instances(),
        "engine_states": extract_states(),
        "rtl_addr_words": extract_localparams(),
        "c_register_offsets": extract_c_register_macros(),
        "fifo_window": extract_fifo_window(),
        "source_root": str(ROOT),
    }
    path = BUILD_DIAGRAMS / "source_model.json"
    write(path, json.dumps(data, indent=2, sort_keys=True))
    return path
