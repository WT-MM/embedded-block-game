#!/usr/bin/env python3
"""Build a readable static diagram gallery.

The source artifacts stay as Mermaid/WaveDrom/Markdown files. This script
places a browser-readable index next to them so the diagrams can be inspected
without opening each raw `.mmd` file by hand.
"""

from __future__ import annotations

from pathlib import Path
import html
import json


ROOT = Path(__file__).resolve().parents[1]
DIAGRAMS = ROOT / "docs" / "diagrams"
OUT_HTML = DIAGRAMS / "index.html"
OUT_MD = DIAGRAMS / "all_diagrams.md"


DIAGRAM_TITLES = {
    "full_system_architecture.mmd": "Full System Architecture",
    "hps_fpga_ownership.mmd": "HPS/FPGA Ownership",
    "hps_software_architecture.mmd": "HPS Software Architecture",
    "hps_game_loop_flow.mmd": "HPS Game Loop Flow",
    "hps_world_streaming_mesh_flow.mmd": "HPS World Streaming / Mesh Flow",
    "hps_interaction_logic_flow.mmd": "HPS Interaction Logic Flow",
    "hps_to_fpga_dataflow.mmd": "HPS-to-FPGA Dataflow",
    "register_interface_flow.mmd": "Register Interface Flow",
    "soc_system_context.mmd": "soc_system Context",
    "voxel_gpu_module_hierarchy.mmd": "voxel_gpu Module Hierarchy",
    "voxel_gpu_datapath.mmd": "voxel_gpu Datapath",
    "voxel_gpu_pipeline.mmd": "voxel_gpu Pipeline",
    "voxel_gpu_control_fsm.mmd": "voxel_gpu Control FSM",
    "memory_and_buffer_ownership.mmd": "Memory and Buffer Ownership",
    "game_to_pixels_flow.mmd": "Game-to-Pixels Flow",
    "c_call_graph.mmd": "C Call Graph",
}


DOC_TITLES = {
    "diagram_index.md": "Diagram Index",
    "hardware_software_interface.md": "Hardware/Software Interface",
    "operator_level_datapaths.md": "Operator-Level Datapaths",
    "register_map.md": "Register Map",
    "source_traceability.md": "Source Traceability",
    "../hps_game_logic_breakdown.md": "HPS Game Logic Breakdown",
    "../final_breakdown.md": "Final Breakdown",
    "../file_listings.md": "File Listings",
}


SVG_TITLES = {
    "raster_setup_operator_datapath.svg": "Raster Setup Operator Datapath",
    "raster_draw_step_operator_datapath.svg": "Raster Draw-Step Operator Datapath",
    "texture_pipeline_operator_datapath.svg": "Perspective Texture / Palette / Fog Operators",
    "memory_access_operator_datapath.svg": "Framebuffer / Z / SDRAM Memory Operators",
}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def anchor_for(name: str) -> str:
    keep = []
    for char in name.lower():
        if char.isalnum():
            keep.append(char)
        elif char in {"_", "-", ".", " "}:
            keep.append("-")
    return "".join(keep).strip("-")


def mermaid_sections() -> str:
    sections = []
    for path in sorted(DIAGRAMS.glob("*.mmd")):
        name = path.name
        title = DIAGRAM_TITLES.get(name, path.stem.replace("_", " ").title())
        anchor = anchor_for(name)
        body = html.escape(read(path))
        sections.append(
            f"""
            <section class="diagram-card" id="{anchor}">
              <div class="card-header">
                <div>
                  <h2>{html.escape(title)}</h2>
                  <a href="{html.escape(name)}">{html.escape(name)}</a>
                </div>
              </div>
              <div class="diagram-scroll">
                <pre class="mermaid">{body}</pre>
              </div>
            </section>
            """
        )
    return "\n".join(sections)


def timing_section() -> str:
    path = DIAGRAMS / "voxel_gpu_timing.wave.json"
    svg = DIAGRAMS / "voxel_gpu_timing.svg"
    if not path.exists():
        return ""
    raw = read(path)
    try:
        parsed = json.dumps(json.loads(raw), indent=2)
    except json.JSONDecodeError:
        parsed = raw
    escaped_json = html.escape(parsed)
    script_json = html.escape(parsed, quote=False)
    rendered = ""
    if svg.exists():
        rendered = """
        <div class="timing-image">
          <img src="voxel_gpu_timing.svg" alt="Rendered voxel_gpu timing diagram">
        </div>
        """
    return f"""
      <section class="diagram-card" id="voxel-gpu-timing-wave-json">
        <div class="card-header">
          <div>
            <h2>voxel_gpu Timing WaveDrom</h2>
            <a href="voxel_gpu_timing.wave.json">voxel_gpu_timing.wave.json</a>
          </div>
        </div>
        {rendered}
        <div class="wavedrom-render">
          <script type="WaveDrom">
{script_json}
          </script>
        </div>
        <details>
          <summary>WaveJSON source</summary>
          <pre class="source-block">{escaped_json}</pre>
        </details>
      </section>
    """


def svg_sections() -> str:
    sections = []
    for name, title in SVG_TITLES.items():
        path = DIAGRAMS / name
        if not path.exists():
            continue
        anchor = anchor_for(name)
        sections.append(
            f"""
            <section class="diagram-card" id="{anchor}">
              <div class="card-header">
                <div>
                  <h2>{html.escape(title)}</h2>
                  <a href="{html.escape(name)}">{html.escape(name)}</a>
                </div>
              </div>
              <div class="svg-image">
                <img src="{html.escape(name)}" alt="{html.escape(title)}">
              </div>
            </section>
            """
        )
    return "\n".join(sections)


def markdown_bundle() -> str:
    lines = [
        "# All Diagrams",
        "",
        "Browser-rendered gallery: [index.html](index.html).",
        "",
        "Supporting docs:",
    ]
    for href, title in DOC_TITLES.items():
        target = DIAGRAMS / href
        if target.exists():
            lines.append(f"- [{title}]({href})")
    lines.append("")

    for path in sorted(DIAGRAMS.glob("*.mmd")):
        title = DIAGRAM_TITLES.get(path.name, path.stem.replace("_", " ").title())
        lines.extend(
            [
                f"## {title}",
                "",
                f"Source: [{path.name}]({path.name})",
                "",
                "```mermaid",
                read(path).rstrip(),
                "```",
                "",
            ]
        )

    for name, title in SVG_TITLES.items():
        path = DIAGRAMS / name
        if not path.exists():
            continue
        lines.extend(
            [
                f"## {title}",
                "",
                f"Rendered SVG: [{name}]({name})",
                "",
                f"![{title}]({name})",
                "",
            ]
        )

    timing = DIAGRAMS / "voxel_gpu_timing.wave.json"
    if timing.exists():
        lines.extend(
            [
                "## voxel_gpu Timing WaveDrom",
                "",
                "Rendered SVG: [voxel_gpu_timing.svg](voxel_gpu_timing.svg)",
                "",
                "![voxel_gpu timing](voxel_gpu_timing.svg)",
                "",
                "Source: [voxel_gpu_timing.wave.json](voxel_gpu_timing.wave.json)",
                "",
                "```json",
                read(timing).rstrip(),
                "```",
                "",
            ]
        )
    return "\n".join(lines)


def nav() -> str:
    diagram_links = []
    for path in sorted(DIAGRAMS.glob("*.mmd")):
        title = DIAGRAM_TITLES.get(path.name, path.stem.replace("_", " ").title())
        diagram_links.append(f'<a href="#{anchor_for(path.name)}">{html.escape(title)}</a>')
    for name, title in SVG_TITLES.items():
        if (DIAGRAMS / name).exists():
            diagram_links.append(f'<a href="#{anchor_for(name)}">{html.escape(title)}</a>')
    diagram_links.append('<a href="#voxel-gpu-timing-wave-json">voxel_gpu Timing WaveDrom</a>')

    doc_links = []
    for href, title in DOC_TITLES.items():
        target = DIAGRAMS / href
        if target.exists():
            doc_links.append(f'<a href="{html.escape(href)}">{html.escape(title)}</a>')

    return f"""
      <aside class="sidebar">
        <h2>Diagrams</h2>
        <nav>{''.join(diagram_links)}</nav>
        <h2>Docs</h2>
        <nav>{''.join(doc_links)}</nav>
      </aside>
    """


def main() -> int:
    DIAGRAMS.mkdir(parents=True, exist_ok=True)
    content = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Embedded Block Game Diagrams</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f7f8fb;
      --panel: #ffffff;
      --ink: #18202d;
      --muted: #5b6575;
      --line: #d9dee8;
      --accent: #255f9f;
      --accent-soft: #e7f0fb;
      --code: #f2f5f9;
    }}
    * {{ box-sizing: border-box; }}
    html {{ scroll-behavior: smooth; }}
    body {{
      margin: 0;
      background: var(--bg);
      color: var(--ink);
      font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }}
    header {{
      padding: 28px 32px 20px;
      border-bottom: 1px solid var(--line);
      background: var(--panel);
    }}
    header h1 {{
      margin: 0 0 6px;
      font-size: 28px;
      letter-spacing: 0;
    }}
    header p {{
      margin: 0;
      color: var(--muted);
      max-width: 980px;
    }}
    .layout {{
      display: grid;
      grid-template-columns: 280px minmax(0, 1fr);
      min-height: calc(100vh - 112px);
    }}
    .sidebar {{
      position: sticky;
      top: 0;
      align-self: start;
      height: 100vh;
      overflow: auto;
      padding: 18px;
      border-right: 1px solid var(--line);
      background: #fbfcfe;
    }}
    .sidebar h2 {{
      margin: 12px 0 8px;
      font-size: 12px;
      text-transform: uppercase;
      color: var(--muted);
      letter-spacing: 0.06em;
    }}
    .sidebar nav {{
      display: grid;
      gap: 4px;
    }}
    a {{
      color: var(--accent);
      text-decoration: none;
    }}
    a:hover {{ text-decoration: underline; }}
    .sidebar a {{
      display: block;
      padding: 6px 8px;
      border-radius: 6px;
      color: var(--ink);
    }}
    .sidebar a:hover {{
      background: var(--accent-soft);
      text-decoration: none;
    }}
    main {{
      padding: 24px;
      display: grid;
      gap: 24px;
    }}
    .diagram-card {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      overflow: hidden;
      box-shadow: 0 1px 2px rgba(22, 34, 51, 0.04);
    }}
    .card-header {{
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 16px;
      padding: 14px 18px;
      border-bottom: 1px solid var(--line);
      background: #fbfcfe;
    }}
    .card-header h2 {{
      margin: 0 0 2px;
      font-size: 18px;
      letter-spacing: 0;
    }}
    .card-header a {{
      font-size: 13px;
      color: var(--muted);
    }}
    .diagram-scroll {{
      overflow: auto;
      padding: 18px;
      min-height: 280px;
    }}
    .mermaid {{
      margin: 0;
      min-width: 820px;
      background: transparent;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 14px;
    }}
    .wavedrom-render {{
      overflow: auto;
      padding: 18px;
      min-height: 180px;
    }}
    .timing-image {{
      overflow: auto;
      padding: 18px;
      border-bottom: 1px solid var(--line);
      background: #ffffff;
    }}
    .svg-image {{
      overflow: auto;
      padding: 18px;
      background: #ffffff;
    }}
    .timing-image img,
    .svg-image img {{
      display: block;
      max-width: none;
      height: auto;
    }}
    details {{
      border-top: 1px solid var(--line);
      padding: 12px 18px 18px;
    }}
    summary {{
      cursor: pointer;
      color: var(--accent);
      font-weight: 600;
    }}
    .source-block {{
      overflow: auto;
      padding: 12px;
      background: var(--code);
      border: 1px solid var(--line);
      border-radius: 6px;
      font-size: 13px;
    }}
    @media (max-width: 900px) {{
      .layout {{ grid-template-columns: 1fr; }}
      .sidebar {{
        position: static;
        height: auto;
        border-right: 0;
        border-bottom: 1px solid var(--line);
      }}
      .sidebar nav {{
        grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
      }}
      main {{ padding: 16px; }}
      header {{ padding: 22px 18px; }}
    }}
  </style>
</head>
<body>
  <header>
    <h1>Embedded Block Game Diagrams</h1>
    <p>Rendered view of the source-grounded architecture, HPS/FPGA interface, datapath, pipeline, register, and timing diagram artifacts generated by the project workflow.</p>
  </header>
  <div class="layout">
    {nav()}
    <main>
      {mermaid_sections()}
      {svg_sections()}
      {timing_section()}
    </main>
  </div>
  <script type="module">
    import mermaid from "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs";
    mermaid.initialize({{
      startOnLoad: true,
      securityLevel: "loose",
      theme: "default",
      flowchart: {{ htmlLabels: true, curve: "basis", useMaxWidth: false }}
    }});
  </script>
  <script src="https://unpkg.com/wavedrom@3.5.0/skins/default.js"></script>
  <script src="https://unpkg.com/wavedrom@3.5.0/wavedrom.min.js"></script>
  <script>
    window.addEventListener("load", function () {{
      if (window.WaveDrom) {{
        window.WaveDrom.ProcessAll();
      }}
    }});
  </script>
</body>
</html>
"""
    OUT_HTML.write_text(content, encoding="utf-8")
    OUT_MD.write_text(markdown_bundle(), encoding="utf-8")
    print(OUT_HTML)
    print(OUT_MD)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
