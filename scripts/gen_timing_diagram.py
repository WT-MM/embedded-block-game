#!/usr/bin/env python3
"""Generate WaveDrom timing JSON from a VCD when available.

If no VCD exists, emit a clearly marked skeleton. The skeleton is intentional:
the project did not have a dedicated testbench directory, and this workflow
must not invent timing behavior.
"""

from __future__ import annotations

from pathlib import Path
import json
import re


ROOT = Path(__file__).resolve().parents[1]
VCD = ROOT / "build" / "diagrams" / "voxel_gpu.vcd"
OUT = ROOT / "docs" / "diagrams" / "voxel_gpu_timing.wave.json"

TARGET_SUFFIXES = [
    "clk",
    "reset",
    "chipselect",
    "write",
    "address",
    "writedata",
    "readdata",
    "dut.state",
    "dut.fifo_count",
    "dut.pipe0_valid",
    "dut.recip0_valid",
    "dut.recip1_valid",
    "dut.recip2_valid",
    "dut.pipe1_valid",
    "dut.tex0_valid",
    "dut.pipe2_valid",
    "dut.draw_pipe_valid",
    "dut.pal_rd_valid",
    "dut.plr_valid",
    "dut.fog0_valid",
    "dut.fog1_valid",
    "dut.commit_valid",
    "dut.commit_valid_o",
    "VGA_VS",
]


def skeleton() -> dict:
    return {
        "head": {
            "text": "voxel_gpu timing skeleton - requires real simulation VCD",
            "tick": 0,
        },
        "signal": [
            {"name": "clk", "wave": "p........", "node": "."},
            {"name": "reset", "wave": "10.......", "data": ["driven by tb/voxel_gpu_tb.sv"]},
            {"name": "chipselect/write", "wave": "0........", "data": ["no real transaction captured"]},
            {"name": "address", "wave": "x........", "data": ["requires VCD"]},
            {"name": "writedata", "wave": "x........", "data": ["requires VCD"]},
            {"name": "state", "wave": "x........", "data": ["requires VCD from dut.state"]},
            {"name": "pipe0_valid", "wave": "x........", "data": ["requires descriptor stimulus"]},
            {"name": "draw_pipe_valid", "wave": "x........", "data": ["requires descriptor stimulus"]},
            {"name": "commit_valid", "wave": "x........", "data": ["requires descriptor stimulus"]},
            {"name": "VGA_VS", "wave": "x........", "data": ["requires running scanout long enough"]},
        ],
        "foot": {
            "text": "No timing behavior is asserted without build/diagrams/voxel_gpu.vcd.",
        },
    }


def parse_vcd(path: Path) -> dict:
    id_to_name: dict[str, str] = {}
    id_to_width: dict[str, int] = {}
    scopes: list[str] = []
    events: list[tuple[int, str, str]] = []
    current_time = 0
    in_defs = True

    var_re = re.compile(r"\$var\s+\w+\s+(\d+)\s+(\S+)\s+(\S+)")
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            if in_defs:
                if line.startswith("$scope"):
                    parts = line.split()
                    if len(parts) >= 3:
                        scopes.append(parts[2])
                elif line.startswith("$upscope"):
                    if scopes:
                        scopes.pop()
                elif line.startswith("$var"):
                    match = var_re.match(line)
                    if match:
                        width = int(match.group(1))
                        ident = match.group(2)
                        name = match.group(3)
                        full = ".".join(scopes + [name])
                        id_to_name[ident] = full
                        id_to_width[ident] = width
                elif line.startswith("$enddefinitions"):
                    in_defs = False
                continue
            if line.startswith("#"):
                current_time = int(line[1:])
                continue
            if line[0] in "01xzXZ":
                ident = line[1:]
                if ident in id_to_name:
                    events.append((current_time, ident, line[0].lower()))
            elif line[0] in "bBrR":
                parts = line.split()
                if len(parts) == 2 and parts[1] in id_to_name:
                    events.append((current_time, parts[1], parts[0][1:].lower()))

    selected: dict[str, str] = {}
    for ident, name in id_to_name.items():
        for suffix in TARGET_SUFFIXES:
            if name.endswith(suffix):
                selected[ident] = name
                break

    if not selected or not events:
        return skeleton()

    times = sorted({time for time, ident, _ in events if ident in selected})[:48]
    if not times:
        return skeleton()

    changes: dict[str, dict[int, str]] = {ident: {} for ident in selected}
    for time, ident, value in events:
        if ident in selected and time in times:
            changes[ident][time] = value

    signals = []
    for ident, name in sorted(selected.items(), key=lambda item: item[1]):
        width = id_to_width.get(ident, 1)
        prev = "x"
        wave = ""
        data = []
        for time in times:
            value = changes[ident].get(time, prev)
            if width == 1:
                symbol = value if value in {"0", "1", "x", "z"} else "x"
                wave += symbol if not wave or symbol != prev else "."
                prev = symbol
            else:
                if value == prev:
                    wave += "."
                else:
                    wave += "="
                    data.append(value)
                    prev = value
        short_name = name
        if ".dut." in short_name:
            short_name = short_name.split(".dut.", 1)[1]
        elif "." in short_name:
            short_name = short_name.rsplit(".", 1)[1]
        entry = {"name": short_name, "wave": wave}
        if data:
            entry["data"] = data
        signals.append(entry)

    return {
        "head": {"text": f"voxel_gpu timing derived from {VCD.relative_to(ROOT)}"},
        "signal": signals,
        "foot": {"text": "Signals are sampled from the VCD; absent signals are not inferred."},
    }


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    data = parse_vcd(VCD) if VCD.exists() else skeleton()
    OUT.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

