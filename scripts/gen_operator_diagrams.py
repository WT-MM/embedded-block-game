#!/usr/bin/env python3
"""Generate operator-level SVG schematics for voxel_gpu.

These diagrams are deliberately closer to a hardware whiteboard sketch than to
an architecture box diagram: they show MUL/ADD/SUB/CMP/SHIFT/MUX/RAM/REG blocks
and the source-visible signals that pass through them. The content is
source-grounded in the RTL files listed in OPERATOR_DIAGRAMS below.
"""

from __future__ import annotations

from pathlib import Path
import html


ROOT = Path(__file__).resolve().parents[1]
DIAGRAMS = ROOT / "docs" / "diagrams"


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


class Svg:
    def __init__(self, width: int, height: int, title: str, subtitle: str) -> None:
        self.width = width
        self.height = height
        self.parts: list[str] = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
            "<defs>",
            '<marker id="arrow" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth">',
            '<path d="M0,0 L0,6 L9,3 z" fill="#344054"/>',
            "</marker>",
            '<marker id="arrow-blue" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth">',
            '<path d="M0,0 L0,6 L9,3 z" fill="#255f9f"/>',
            "</marker>",
            '<marker id="arrow-red" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth">',
            '<path d="M0,0 L0,6 L9,3 z" fill="#9b2c4b"/>',
            "</marker>",
            "</defs>",
            "<style>",
            "text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; fill: #18202d; }",
            ".mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }",
            ".small { font-size: 12px; fill: #5b6575; }",
            ".tiny { font-size: 10.5px; fill: #667085; }",
            ".title { font-size: 22px; font-weight: 700; }",
            ".subtitle { font-size: 13px; fill: #5b6575; }",
            "</style>",
            f'<rect x="0" y="0" width="{width}" height="{height}" fill="#fbfcfe"/>',
            f'<text x="28" y="34" class="title">{esc(title)}</text>',
            f'<text x="28" y="56" class="subtitle">{esc(subtitle)}</text>',
        ]

    def group(self, x: int, y: int, w: int, h: int, title: str, fill: str = "#ffffff") -> None:
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="{fill}" stroke="#d0d7e2" stroke-width="1.2"/>'
        )
        self.parts.append(f'<text x="{x + 14}" y="{y + 24}" font-size="14" font-weight="700" fill="#344054">{esc(title)}</text>')

    def node(
        self,
        x: int,
        y: int,
        w: int,
        h: int,
        label: str,
        kind: str = "logic",
        sub: str | None = None,
    ) -> None:
        colors = {
            "src": ("#eef4ff", "#2f63b8"),
            "op": ("#e6fbff", "#247a91"),
            "mul": ("#e6fbff", "#247a91"),
            "add": ("#e8f7ef", "#2a7a45"),
            "sub": ("#fff4e5", "#b76e00"),
            "cmp": ("#fff3cc", "#b98500"),
            "mux": ("#f6eaff", "#7b4bb2"),
            "reg": ("#fff8db", "#aa8300"),
            "mem": ("#f1ecff", "#6e3fa0"),
            "ctrl": ("#ffe9ef", "#b83a58"),
            "out": ("#f2f4f7", "#667085"),
        }
        fill, stroke = colors.get(kind, colors["op"])
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="7" fill="{fill}" stroke="{stroke}" stroke-width="1.4"/>'
        )
        lines = label.split("\n")
        base_y = y + (h / 2) - (len(lines) - 1) * 8
        for idx, line in enumerate(lines):
            weight = "700" if idx == 0 else "500"
            self.parts.append(
                f'<text x="{x + w / 2:.1f}" y="{base_y + idx * 16:.1f}" text-anchor="middle" font-size="13" font-weight="{weight}">{esc(line)}</text>'
            )
        if sub:
            self.parts.append(
                f'<text x="{x + w / 2:.1f}" y="{y + h + 15}" text-anchor="middle" class="tiny">{esc(sub)}</text>'
            )

    def op(self, x: int, y: int, label: str, kind: str = "op", sub: str | None = None) -> None:
        self.node(x, y, 74, 46, label, kind, sub)

    def reg(self, x: int, y: int, label: str, sub: str | None = None) -> None:
        self.node(x, y, 122, 48, label, "reg", sub)
        self.parts.append(f'<path d="M{x + 8},{y + 40} l8,-8 l8,8" fill="none" stroke="#aa8300" stroke-width="1.2"/>')

    def mem(self, x: int, y: int, w: int, h: int, label: str, sub: str | None = None) -> None:
        self.node(x, y, w, h, label, "mem", sub)
        self.parts.append(f'<path d="M{x + 10},{y + 10} h{w - 20}" stroke="#6e3fa0" stroke-width="1" opacity="0.45"/>')
        self.parts.append(f'<path d="M{x + 10},{y + 20} h{w - 20}" stroke="#6e3fa0" stroke-width="1" opacity="0.28"/>')

    def arrow(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        label: str | None = None,
        color: str = "#344054",
        width: float = 1.5,
    ) -> None:
        marker = "arrow-blue" if color == "#255f9f" else "arrow-red" if color == "#9b2c4b" else "arrow"
        self.parts.append(
            f'<path d="M{x1},{y1} L{x2},{y2}" fill="none" stroke="{color}" stroke-width="{width}" marker-end="url(#{marker})"/>'
        )
        if label:
            self.parts.append(
                f'<text x="{(x1 + x2) / 2:.1f}" y="{(y1 + y2) / 2 - 5:.1f}" text-anchor="middle" class="tiny">{esc(label)}</text>'
            )

    def elbow(
        self,
        points: list[tuple[int, int]],
        label: str | None = None,
        color: str = "#344054",
        width: float = 1.5,
    ) -> None:
        marker = "arrow-blue" if color == "#255f9f" else "arrow-red" if color == "#9b2c4b" else "arrow"
        d = "M" + " L".join(f"{x},{y}" for x, y in points)
        self.parts.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}" marker-end="url(#{marker})"/>')
        if label and len(points) >= 2:
            x1, y1 = points[len(points) // 2 - 1]
            x2, y2 = points[len(points) // 2]
            self.parts.append(
                f'<text x="{(x1 + x2) / 2:.1f}" y="{(y1 + y2) / 2 - 5:.1f}" text-anchor="middle" class="tiny">{esc(label)}</text>'
            )

    def note(self, x: int, y: int, text: str, w: int = 360) -> None:
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="62" rx="8" fill="#ffffff" stroke="#d0d7e2"/>')
        for idx, line in enumerate(text.split("\n")):
            self.parts.append(f'<text x="{x + 12}" y="{y + 22 + idx * 16}" class="small">{esc(line)}</text>')

    def legend(self, x: int, y: int) -> None:
        entries = [("MUL", "mul"), ("ADD", "add"), ("SUB", "sub"), ("CMP", "cmp"), ("MUX", "mux"), ("REG", "reg"), ("RAM/ROM", "mem")]
        self.parts.append(f'<text x="{x}" y="{y}" font-size="13" font-weight="700">Legend</text>')
        for idx, (name, kind) in enumerate(entries):
            xx = x + (idx % 4) * 100
            yy = y + 14 + (idx // 4) * 36
            self.node(xx, yy, 82, 26, name, kind)

    def finish(self) -> str:
        self.parts.append("</svg>")
        return "\n".join(self.parts) + "\n"


def raster_setup_svg() -> str:
    svg = Svg(
        1500,
        900,
        "Raster Setup Operator Datapath",
        "voxel_raster_setup: edge eval = A*x + B*y + C, plus odd-x start correction for z/uw/vw/iw.",
    )
    svg.group(24, 84, 1452, 346, "Edge-equation setup, repeated for i = 0..3", "#ffffff")
    svg.node(56, 132, 152, 54, "draw_x_start_even\n10b -> signed", "src")
    svg.node(56, 230, 152, 54, "draw_y_min\n9b -> signed", "src")
    svg.node(246, 120, 126, 54, "edge_A[i]\n32s", "src")
    svg.node(246, 218, 126, 54, "edge_B[i]\n32s", "src")
    svg.node(246, 318, 126, 54, "edge_C[i]\n32s", "src")
    svg.op(430, 124, "MUL\nA*x", "mul", "edge_ax0..3")
    svg.op(430, 224, "MUL\nB*y", "mul", "edge_by0..3")
    svg.op(610, 174, "ADD\nax+by", "add")
    svg.op(760, 174, "ADD\n+C", "add")
    svg.reg(930, 154, "ST_SETUP\nedge_row_val[i]")
    svg.reg(1090, 154, "ST_SETUP\nedge_cur_val[i]")
    svg.node(1260, 144, 170, 78, "4 edge lanes\nedge_eval0..3", "out", "inside tests consume these")
    svg.arrow(208, 159, 430, 147, "x")
    svg.arrow(372, 147, 430, 147, "A")
    svg.arrow(208, 257, 430, 247, "y")
    svg.arrow(372, 245, 430, 247, "B")
    svg.arrow(504, 147, 610, 197)
    svg.arrow(504, 247, 610, 197)
    svg.arrow(684, 197, 760, 197)
    svg.arrow(372, 345, 760, 215, "C sign-extend")
    svg.arrow(834, 197, 930, 178)
    svg.arrow(1052, 178, 1090, 178)
    svg.arrow(1212, 178, 1260, 183)

    svg.group(24, 462, 1452, 296, "Attribute start correction, one block each for z, uw, vw, iw", "#ffffff")
    svg.node(56, 520, 150, 54, "draw_*_0\nz/uw/vw/iw", "src")
    svg.node(56, 628, 150, 54, "draw_*_dx\nper-pixel delta", "src")
    svg.node(246, 582, 140, 54, "draw_x_min[0]\nodd start?", "src")
    svg.op(438, 592, "MUX\n0 or dx", "mux")
    svg.op(602, 568, "SUB\n*_0 - dx", "sub")
    svg.reg(782, 522, "ST_SETUP\n*_row_val")
    svg.reg(782, 608, "ST_SETUP\n*_cur_val")
    svg.node(970, 552, 180, 86, "Outputs\nz_start_val\nuw/vw/iw_start_val", "out")
    svg.node(1210, 534, 210, 96, "Operator count from RTL\n8 edge MUL\n8 edge ADD\n4 correction SUB/MUX", "ctrl")
    svg.arrow(206, 547, 602, 591, "base")
    svg.arrow(206, 655, 438, 619, "dx")
    svg.arrow(386, 609, 438, 619, "select")
    svg.arrow(512, 619, 602, 591)
    svg.arrow(676, 591, 782, 546)
    svg.arrow(676, 591, 782, 632)
    svg.arrow(904, 546, 970, 576)
    svg.arrow(904, 632, 970, 614)
    svg.note(56, 786, "Source: hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_raster_setup\nRegistered in ST_SETUP in hw/voxel_gpu/rtl/voxel_gpu.sv", 560)
    svg.legend(820, 780)
    return svg.finish()


def draw_step_svg() -> str:
    svg = Svg(
        1560,
        940,
        "Draw-Step / Two-Pixel Raster Walk Operators",
        "voxel_draw_step and ST_DRAW: lane0/lane1 edge tests, bbox checks, cache address, next-pair and next-row increments.",
    )
    svg.group(24, 84, 1512, 330, "Two-pixel issue path", "#ffffff")
    svg.reg(54, 145, "edge_cur_val[0..3]")
    svg.node(54, 250, 122, 48, "edge_A[0..3]", "src")
    svg.op(238, 134, "CMP\n>= 0 x4", "cmp")
    svg.op(382, 134, "AND\ninside0", "ctrl")
    svg.op(238, 248, "ADD\nedge+A x4", "add", "lane1 edge")
    svg.op(382, 248, "CMP\n>= 0 x4", "cmp")
    svg.op(526, 190, "AND\ninside1", "ctrl")
    svg.node(660, 134, 140, 52, "draw_x_cur\nlane0 x", "src")
    svg.node(660, 248, 140, 52, "draw_x_next\nlane1 x", "src")
    svg.op(858, 126, "CMP\nx bounds", "cmp")
    svg.op(858, 240, "CMP\nx bounds", "cmp")
    svg.op(1032, 182, "AND\ninside & bounds", "ctrl")
    svg.reg(1200, 166, "pipe0 / pipe0_o\nvalid + attrs")
    svg.node(1382, 160, 110, 60, "to recip\npipeline", "out")
    svg.arrow(176, 169, 238, 157)
    svg.arrow(176, 169, 238, 271)
    svg.arrow(176, 274, 238, 271)
    svg.arrow(312, 157, 382, 157)
    svg.arrow(312, 271, 382, 271)
    svg.arrow(456, 157, 526, 213)
    svg.arrow(456, 271, 526, 213)
    svg.arrow(800, 160, 858, 149)
    svg.arrow(800, 274, 858, 263)
    svg.arrow(932, 149, 1032, 205)
    svg.arrow(932, 263, 1032, 205)
    svg.arrow(600, 213, 1032, 205)
    svg.arrow(1106, 205, 1200, 190)
    svg.arrow(1322, 190, 1382, 190)

    svg.group(24, 450, 1512, 328, "Coordinate, depth, and attribute increments", "#ffffff")
    svg.reg(58, 510, "cur registers\nedge/z/uw/vw/iw")
    svg.reg(58, 638, "row registers\nedge/z/uw/vw/iw")
    svg.node(240, 500, 132, 58, "dx deltas\nA,dz_dx,uw_dx...", "src")
    svg.node(240, 628, 132, 58, "dy deltas\nB,dz_dy,uw_dy...", "src")
    svg.op(444, 492, "ADD\ncur + dx", "add", "lane1")
    svg.op(592, 492, "ADD\n+ dx", "add", "next pair")
    svg.op(444, 628, "ADD\nrow + dy", "add", "next row")
    svg.op(750, 548, "MUX\npair/row/flush", "mux")
    svg.reg(926, 486, "edge_cur_val\nz_cur_val\nuw/vw/iw_cur")
    svg.reg(926, 630, "edge_row_val\nz_row_val\nuw/vw/iw_row")
    svg.op(1134, 560, "ADD\nx += 2", "add")
    svg.op(1280, 560, "ADD\ny += 1", "add")
    svg.node(1398, 536, 104, 78, "row exit\nlast/exited", "ctrl")
    svg.arrow(180, 534, 444, 515)
    svg.arrow(372, 529, 444, 515)
    svg.arrow(518, 515, 592, 515)
    svg.arrow(666, 515, 750, 571)
    svg.arrow(180, 662, 444, 651)
    svg.arrow(372, 657, 444, 651)
    svg.arrow(518, 651, 750, 571)
    svg.arrow(824, 571, 926, 510)
    svg.arrow(824, 571, 926, 654)
    svg.arrow(1048, 510, 1134, 583)
    svg.arrow(1048, 654, 1280, 583)
    svg.arrow(1208, 583, 1398, 560)
    svg.arrow(1354, 583, 1398, 590)
    svg.note(52, 810, "Source: hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_draw_step and ST_DRAW register updates in voxel_gpu.sv.\nThe odd lane is explicit in RTL via *_o signals and draw_x_next = draw_x_cur + 1.", 720)
    svg.legend(900, 808)
    return svg.finish()


def texture_pipeline_svg() -> str:
    svg = Svg(
        1740,
        980,
        "Perspective Texture / Palette / Fog Operator Pipeline",
        "pipe0 through commit: normalize 1/w, reciprocal LUT/interpolate, UV multiply, texture ROM, palette/fog, z-test commit.",
    )
    svg.group(24, 84, 1692, 220, "Reciprocal and perspective divide, duplicated for lane0 and lane1 (_o)", "#ffffff")
    svg.reg(52, 150, "pipe0\nuw,vw,iw,z,addr")
    svg.op(210, 152, "MSB\nscan", "cmp")
    svg.op(324, 152, "SHIFT\nnormalize iw", "op")
    svg.mem(456, 132, 130, 86, "recip_lut\n2 reads", "lo/hi")
    svg.op(634, 152, "SUB\nlo-hi", "sub")
    svg.op(752, 152, "MUL\n* frac", "mul")
    svg.op(872, 152, "ADD/SHIFT\n+32 >> 6", "add")
    svg.op(1012, 152, "SUB\nlo-step", "sub")
    svg.op(1140, 152, "SHIFT\ndenorm", "op")
    svg.reg(1274, 150, "pipe1\nw_q aligned")
    svg.op(1436, 128, "MUL\nuw*w", "mul")
    svg.op(1436, 190, "MUL\nvw*w", "mul")
    svg.reg(1574, 150, "tex0\nu_prod/v_prod")
    for x1, y1, x2, y2 in [
        (174, 174, 210, 175),
        (284, 175, 324, 175),
        (398, 175, 456, 175),
        (586, 175, 634, 175),
        (708, 175, 752, 175),
        (826, 175, 872, 175),
        (946, 175, 1012, 175),
        (1086, 175, 1140, 175),
        (1214, 175, 1274, 174),
        (1396, 174, 1436, 151),
        (1396, 174, 1436, 213),
        (1510, 151, 1574, 174),
        (1510, 213, 1574, 190),
    ]:
        svg.arrow(x1, y1, x2, y2)

    svg.group(24, 340, 1692, 250, "Texture address and color path", "#ffffff")
    svg.op(70, 420, "FUNC\ntexture_coord", "op", "repeat/clip")
    svg.op(220, 420, "CONCAT\n{tile,v,u}", "op")
    svg.reg(366, 420, "pipe2\ntex_addr")
    svg.mem(540, 392, 164, 94, "texture_rom\nA/B ports", "1-cycle altsyncram ROM")
    svg.reg(770, 420, "draw_pipe\nmetadata")
    svg.op(936, 420, "MUX\ntexel/color", "mux")
    svg.op(1060, 420, "FUNC\napply_light_bank", "op")
    svg.reg(1218, 420, "pal_rd\npalette addr")
    svg.mem(1382, 392, 148, 94, "palette +\nsky_palette", "RGB888")
    svg.reg(1582, 420, "plr\nRGB565")
    svg.arrow(144, 443, 220, 443)
    svg.arrow(294, 443, 366, 443)
    svg.arrow(488, 443, 540, 439)
    svg.arrow(704, 439, 770, 443, "tex_rd_data")
    svg.arrow(892, 443, 936, 443)
    svg.arrow(1010, 443, 1060, 443)
    svg.arrow(1134, 443, 1218, 443)
    svg.arrow(1340, 443, 1382, 439)
    svg.arrow(1530, 439, 1582, 443)

    svg.group(24, 630, 1692, 220, "Fog, alpha, and Z/color commit", "#ffffff")
    svg.op(70, 690, "SUB\nx-320", "sub")
    svg.op(70, 760, "SUB\n240-y", "sub")
    svg.op(204, 690, "MUL\ndx*dx", "mul")
    svg.op(204, 760, "MUL\ndy*dy", "mul")
    svg.op(340, 724, "ADD\nradius_sq", "add")
    svg.op(474, 724, "MUL\n* inv_proj_sq", "mul")
    svg.op(626, 724, "MUL/SHIFT\n*3 >> 3", "mul")
    svg.op(790, 724, "ADD\n1.0 + scale", "add")
    svg.op(936, 724, "MUL\nw * scale", "mul")
    svg.reg(1082, 724, "fog0/fog1\nradial_q8.8")
    svg.node(1244, 692, 118, 68, "voxel_fog_blend\nblend/alpha", "op")
    svg.op(1402, 724, "CMP\nz < z_ref", "cmp")
    svg.reg(1540, 724, "commit\naddr/z/color")
    svg.arrow(144, 713, 204, 713)
    svg.arrow(144, 783, 204, 783)
    svg.arrow(278, 713, 340, 747)
    svg.arrow(278, 783, 340, 747)
    svg.arrow(414, 747, 474, 747)
    svg.arrow(548, 747, 626, 747)
    svg.arrow(700, 747, 790, 747)
    svg.arrow(864, 747, 936, 747)
    svg.arrow(1010, 747, 1082, 747)
    svg.arrow(1204, 747, 1244, 726)
    svg.arrow(1362, 726, 1402, 747)
    svg.arrow(1476, 747, 1540, 747)
    svg.note(56, 884, "Source: voxel_math_utils.sv and voxel_gpu.sv pipe0/recip*/pipe1/tex0/pipe2/draw_pipe/pal_rd/plr/fog*/commit signals.", 820)
    svg.legend(1020, 882)
    return svg.finish()


def memory_access_svg() -> str:
    svg = Svg(
        1700,
        1060,
        "Framebuffer / Z / SDRAM Memory Access Operators",
        "Active ping-pong cache reads feed z-test/commit; inactive/cache-maintenance paths flush/load through Sdram_Control.",
    )
    svg.group(24, 84, 1652, 276, "Active draw-cache read path", "#ffffff")
    svg.node(58, 150, 150, 58, "draw_cache_sel\nA/B active", "ctrl")
    svg.op(258, 132, "MUX\nFB A/B", "mux")
    svg.op(258, 224, "MUX\nZ A/B", "mux")
    svg.mem(420, 118, 178, 86, "FB cache A/B\nbanked even/odd", "fb_draw_rd_data_e/o")
    svg.mem(420, 220, 178, 86, "Z cache A/B\nbanked even/odd", "z_draw_rd_data_e/o")
    svg.reg(676, 146, "recip1\nz_ref + dst_rgb")
    svg.op(846, 168, "CMP\nz < z_ref", "cmp")
    svg.node(992, 146, 128, 72, "draw_commit_pass\ninside & alpha & z", "ctrl")
    svg.reg(1190, 146, "commit\naddr/z/color")
    svg.arrow(208, 179, 258, 155)
    svg.arrow(208, 179, 258, 247)
    svg.arrow(332, 155, 420, 161)
    svg.arrow(332, 247, 420, 263)
    svg.arrow(598, 161, 676, 170, "dst_rgb565")
    svg.arrow(598, 263, 676, 190, "z_ref")
    svg.arrow(798, 170, 846, 191)
    svg.arrow(920, 191, 992, 182)
    svg.arrow(1120, 182, 1190, 170)

    svg.group(24, 398, 1652, 286, "Commit write steering into even/odd banks", "#ffffff")
    svg.node(58, 462, 150, 58, "commit_valid\ncommit_pass", "ctrl")
    svg.op(260, 438, "DEMUX\naddr[0]", "mux")
    svg.node(412, 420, 130, 58, "even lane\ncommit_addr", "out")
    svg.node(412, 512, 130, 58, "odd lane\ncommit_addr_o", "out")
    svg.mem(612, 408, 184, 76, "FB RAM\nwr_en_e/o", "commit_color")
    svg.mem(612, 512, 184, 76, "Z RAM\nwr_en_e/o", "commit_z or far")
    svg.op(874, 460, "MUX\nA or B cache", "mux")
    svg.node(1052, 430, 170, 88, "draw_cache_sel chooses\nresident cache", "ctrl")
    svg.mem(1308, 416, 210, 102, "voxel_banked_sdp_ram\nA/B, even/odd banks", "addr[0] selects bank")
    svg.arrow(208, 491, 260, 461)
    svg.arrow(334, 461, 412, 449)
    svg.arrow(334, 461, 412, 541)
    svg.arrow(542, 449, 612, 446)
    svg.arrow(542, 541, 612, 550)
    svg.arrow(796, 446, 874, 483)
    svg.arrow(796, 550, 874, 483)
    svg.arrow(948, 483, 1052, 474)
    svg.arrow(1222, 474, 1308, 467)

    svg.group(24, 720, 1652, 200, "Cache maintenance, SDRAM copy, and scanout", "#ffffff")
    svg.node(58, 782, 150, 58, "cache_flush_state\ncache_load_state", "ctrl")
    svg.mem(258, 760, 176, 86, "inactive cache\nflush/load ports", "flush_cache_sel")
    svg.op(500, 780, "MUX\ncolor/Z/sky", "mux")
    svg.node(652, 760, 150, 86, "WR FIFO push\nsdram_wr_push", "ctrl")
    svg.mem(872, 748, 184, 110, "Sdram_Control\nboard SDRAM", "WR/RD bursts")
    svg.node(1128, 760, 150, 86, "RD pop\ncache or scanout", "ctrl")
    svg.mem(1328, 748, 156, 110, "line buffers\ntriple", "VGA scan")
    svg.node(1534, 770, 104, 76, "VGA\nRGB/HS/VS", "out")
    svg.arrow(208, 811, 258, 803)
    svg.arrow(434, 803, 500, 803)
    svg.arrow(574, 803, 652, 803)
    svg.arrow(802, 803, 872, 803)
    svg.arrow(1056, 803, 1128, 803)
    svg.arrow(1278, 803, 1328, 803)
    svg.arrow(1484, 803, 1534, 808)
    svg.note(50, 944, "Source: voxel_gpu.sv ping-pong cache muxing, banked RAM instances, commit write fanout, Sdram_Control, and scanout control wires.", 820)
    svg.legend(1018, 942)
    return svg.finish()


OPERATOR_DIAGRAMS = [
    {
        "file": "raster_setup_operator_datapath.svg",
        "title": "Raster Setup Operator Datapath",
        "question": "How edge equations and starting z/uw/vw/iw values are computed before ST_DRAW.",
        "sources": [
            "hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_raster_setup",
            "hw/voxel_gpu/rtl/voxel_gpu.sv::ST_SETUP",
        ],
        "svg": raster_setup_svg,
    },
    {
        "file": "raster_draw_step_operator_datapath.svg",
        "title": "Raster Draw-Step Operator Datapath",
        "question": "How the rasterizer advances two pixels per cycle and updates pair/row state.",
        "sources": [
            "hw/voxel_gpu/rtl/voxel_math_utils.sv::voxel_draw_step",
            "hw/voxel_gpu/rtl/voxel_gpu.sv::ST_DRAW",
        ],
        "svg": draw_step_svg,
    },
    {
        "file": "texture_pipeline_operator_datapath.svg",
        "title": "Perspective Texture / Palette / Fog Operator Pipeline",
        "question": "Where reciprocal, UV multiplication, texture address, palette, fog, and z-test operators sit.",
        "sources": [
            "hw/voxel_gpu/rtl/voxel_math_utils.sv",
            "hw/voxel_gpu/rtl/voxel_gpu.sv::pipe0..commit",
            "hw/voxel_gpu/rtl/voxel_texture_rom.sv",
        ],
        "svg": texture_pipeline_svg,
    },
    {
        "file": "memory_access_operator_datapath.svg",
        "title": "Framebuffer / Z / SDRAM Memory Access Operators",
        "question": "How active cache reads, z-test commit writes, bank steering, SDRAM flush/load, and VGA scanout connect.",
        "sources": [
            "hw/voxel_gpu/rtl/voxel_gpu.sv::cache muxing and commit fanout",
            "hw/voxel_gpu/rtl/voxel_sdp_ram.sv",
            "hw/sdram_local_test/Sdram_Control.v",
        ],
        "svg": memory_access_svg,
    },
]


def markdown_index() -> str:
    lines = [
        "# Operator-Level Datapath Diagrams",
        "",
        "These rendered SVGs show the arithmetic, comparison, mux, register, and memory operators used by the rasterization and memory-access paths. They are source-grounded schematics, not Quartus resource reports or post-synthesis netlists.",
        "",
    ]
    for item in OPERATOR_DIAGRAMS:
        lines.extend(
            [
                f"## {item['title']}",
                "",
                f"Answers: {item['question']}",
                "",
                f"![{item['title']}]({item['file']})",
                "",
                "Sources:",
            ]
        )
        for source in item["sources"]:
            lines.append(f"- `{source}`")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    DIAGRAMS.mkdir(parents=True, exist_ok=True)
    outputs = []
    for item in OPERATOR_DIAGRAMS:
        path = DIAGRAMS / item["file"]
        path.write_text(item["svg"](), encoding="utf-8")
        outputs.append(path)
    md = DIAGRAMS / "operator_level_datapaths.md"
    md.write_text(markdown_index(), encoding="utf-8")
    outputs.append(md)
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
