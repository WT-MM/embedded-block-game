#!/usr/bin/env python3
"""Render docs/diagrams/voxel_gpu_timing.wave.json to a static SVG.

This is intentionally small and dependency-free. It supports the WaveDrom
features produced by scripts/gen_timing_diagram.py: scalar 0/1/x/z waveforms,
clock `p`, repeated `.`, and bus/data segments using `=`.
"""

from __future__ import annotations

from pathlib import Path
import html
import json


ROOT = Path(__file__).resolve().parents[1]
IN_JSON = ROOT / "docs" / "diagrams" / "voxel_gpu_timing.wave.json"
OUT_SVG = ROOT / "docs" / "diagrams" / "voxel_gpu_timing.svg"


LABEL_W = 190
CELL_W = 58
ROW_H = 42
TOP = 78
LEFT = 24
RIGHT = 32
BOTTOM = 70


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def polyline(points: list[tuple[float, float]], **attrs: str) -> str:
    attr_text = " ".join(f'{key.replace("_", "-")}="{esc(value)}"' for key, value in attrs.items())
    point_text = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
    return f'<polyline points="{point_text}" {attr_text}/>'


def rect(x: float, y: float, w: float, h: float, **attrs: str) -> str:
    attr_text = " ".join(f'{key.replace("_", "-")}="{esc(value)}"' for key, value in attrs.items())
    return f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" {attr_text}/>'


def text(x: float, y: float, value: object, **attrs: str) -> str:
    attr_text = " ".join(f'{key.replace("_", "-")}="{esc(val)}"' for key, val in attrs.items())
    return f'<text x="{x:.1f}" y="{y:.1f}" {attr_text}>{esc(value)}</text>'


def signal_count(signals: list[dict]) -> int:
    return max((len(str(signal.get("wave", ""))) for signal in signals), default=1)


def draw_clock(row_y: float, start_x: float, cells: int) -> str:
    high = row_y + 9
    low = row_y + 27
    points: list[tuple[float, float]] = [(start_x, low)]
    for idx in range(cells):
        x0 = start_x + idx * CELL_W
        mid = x0 + CELL_W / 2
        x1 = x0 + CELL_W
        points.extend([(x0, high), (mid, high), (mid, low), (x1, low)])
    return polyline(points, fill="none", stroke="#1f4f82", stroke_width="2", stroke_linejoin="round")


def draw_scalar_segment(
    row_y: float,
    x0: float,
    x1: float,
    previous_level: str,
    level: str,
) -> str:
    high = row_y + 9
    low = row_y + 27
    mid = row_y + 18
    if level == "1":
        y = high
        points = []
        if previous_level in {"0", "z"}:
            points.extend([(x0, low), (x0, y)])
        points.extend([(x0, y), (x1, y)])
        return polyline(points, fill="none", stroke="#1d7037", stroke_width="2")
    if level == "0":
        y = low
        points = []
        if previous_level == "1":
            points.extend([(x0, high), (x0, y)])
        points.extend([(x0, y), (x1, y)])
        return polyline(points, fill="none", stroke="#1d7037", stroke_width="2")
    if level == "z":
        return polyline([(x0, mid), (x1, mid)], fill="none", stroke="#777", stroke_width="2", stroke_dasharray="5 4")
    return (
        rect(x0 + 2, row_y + 7, x1 - x0 - 4, 22, fill="#f7e8ec", stroke="#b83a58", stroke_width="1")
        + text((x0 + x1) / 2, row_y + 23, "X", text_anchor="middle", font_size="12", fill="#7c1830")
    )


def draw_bus_segment(row_y: float, x0: float, x1: float, label: str) -> str:
    return (
        rect(x0 + 2, row_y + 7, x1 - x0 - 4, 22, rx="3", fill="#e8f1ff", stroke="#2f63b8", stroke_width="1.3")
        + text((x0 + x1) / 2, row_y + 23, label or "data", text_anchor="middle", font_size="11", fill="#0d2448")
    )


def render_signal(signal: dict, row_index: int, cells: int) -> str:
    row_y = TOP + row_index * ROW_H
    start_x = LEFT + LABEL_W
    wave = str(signal.get("wave", ""))
    data = [str(item) for item in signal.get("data", [])]
    data_idx = 0
    out = [text(LEFT, row_y + 23, signal.get("name", ""), font_size="13", fill="#18202d")]
    out.append(f'<line x1="{start_x:.1f}" y1="{row_y + 18:.1f}" x2="{start_x + cells * CELL_W:.1f}" y2="{row_y + 18:.1f}" stroke="#edf0f5" stroke-width="1"/>')

    if wave.startswith("p"):
        out.append(draw_clock(row_y, start_x, cells))
    else:
        previous_token = "x"
        current_token = "x"
        for idx in range(cells):
            raw_token = wave[idx] if idx < len(wave) else "."
            if raw_token != ".":
                current_token = raw_token
            x0 = start_x + idx * CELL_W
            x1 = x0 + CELL_W
            if current_token == "=":
                label = data[data_idx] if data_idx < len(data) else ""
                if raw_token == "=":
                    data_idx += 1
                out.append(draw_bus_segment(row_y, x0, x1, label))
            else:
                out.append(draw_scalar_segment(row_y, x0, x1, previous_token, current_token))
            if raw_token != ".":
                previous_token = current_token

    if data and "=" not in wave:
        note = "; ".join(data[:2])
        if len(data) > 2:
            note += "; ..."
        out.append(text(start_x + cells * CELL_W + 10, row_y + 23, note, font_size="12", fill="#5b6575"))
    return "\n".join(out)


def render(data: dict) -> str:
    signals = list(data.get("signal", []))
    cells = max(signal_count(signals), 8)
    width = LEFT + LABEL_W + cells * CELL_W + RIGHT + 280
    height = TOP + len(signals) * ROW_H + BOTTOM
    wave_x0 = LEFT + LABEL_W
    wave_x1 = wave_x0 + cells * CELL_W

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
        "<style>",
        "text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }",
        ".mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }",
        "</style>",
        rect(0, 0, width, height, fill="#ffffff"),
        text(LEFT, 32, data.get("head", {}).get("text", "voxel_gpu timing"), font_size="20", font_weight="700", fill="#18202d"),
        text(LEFT, 55, f"Source: {IN_JSON.relative_to(ROOT)}", font_size="12", fill="#5b6575"),
        text(wave_x0, 55, "cycles / sampled changes", font_size="12", fill="#5b6575"),
    ]

    for idx in range(cells + 1):
        x = wave_x0 + idx * CELL_W
        parts.append(f'<line x1="{x:.1f}" y1="{TOP - 8:.1f}" x2="{x:.1f}" y2="{TOP + len(signals) * ROW_H - 8:.1f}" stroke="#e3e8f0" stroke-width="1"/>')
        if idx < cells:
            parts.append(text(x + CELL_W / 2, TOP - 15, idx, text_anchor="middle", font_size="11", fill="#7a8494"))

    parts.append(f'<line x1="{LEFT:.1f}" y1="{TOP - 8:.1f}" x2="{wave_x1:.1f}" y2="{TOP - 8:.1f}" stroke="#cbd3df" stroke-width="1"/>')
    parts.append(f'<line x1="{LEFT:.1f}" y1="{TOP + len(signals) * ROW_H - 8:.1f}" x2="{wave_x1:.1f}" y2="{TOP + len(signals) * ROW_H - 8:.1f}" stroke="#cbd3df" stroke-width="1"/>')

    for row_index, signal in enumerate(signals):
        parts.append(render_signal(signal, row_index, cells))

    foot = data.get("foot", {}).get("text")
    if foot:
        parts.append(text(LEFT, height - 28, foot, font_size="13", fill="#7c1830"))
    parts.append("</svg>")
    return "\n".join(parts)


def main() -> int:
    if not IN_JSON.exists():
        raise SystemExit(f"ERROR: missing {IN_JSON}")
    data = json.loads(IN_JSON.read_text(encoding="utf-8"))
    OUT_SVG.write_text(render(data), encoding="utf-8")
    print(OUT_SVG)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
