from __future__ import annotations

from array import array
from collections.abc import Iterable
from pathlib import Path

from .protocol import QUAD_FLAG_ALPHA_KEY, QUAD_FLAG_TEX, QUAD_FLAG_ZTEST, QuadDesc

CLEAR_DEPTH = 0xFFFF
TEXTURE_TILE_SIZE = 16
TEXTURE_TILE_COUNT = 64
TEXTURE_BYTES = TEXTURE_TILE_COUNT * TEXTURE_TILE_SIZE * TEXTURE_TILE_SIZE


def load_texture_hex(path: str | Path) -> bytes:
    data = bytearray()
    texture_path = Path(path)

    with texture_path.open("r", encoding="ascii") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                data.append(int(line, 16))
            except ValueError as exc:
                raise ValueError(
                    f"{texture_path}:{line_number}: invalid hex texel {line!r}"
                ) from exc

    if len(data) != TEXTURE_BYTES:
        raise ValueError(
            f"{texture_path} contains {len(data)} texels; expected {TEXTURE_BYTES}"
        )

    return bytes(data)


class VirtualGPU:
    def __init__(
        self, width: int = 320, height: int = 240, textures: bytes | None = None
    ) -> None:
        self.width = width
        self.height = height
        self.pixel_count = width * height
        self._cleared_frame = bytes(self.pixel_count)
        self._cleared_depth = array("H", [CLEAR_DEPTH]) * self.pixel_count
        self.front_buffer = bytearray(self.pixel_count)
        self.back_buffer = bytearray(self.pixel_count)
        self.z_buffer = self._cleared_depth[:]
        self.palette: list[tuple[int, int, int]] = [(0, 0, 0)] * 256
        self.frame_count = 0
        self.vsync_latch = 0
        self.palette_dirty = True
        if textures is None:
            textures = bytes(TEXTURE_BYTES)
        if len(textures) != TEXTURE_BYTES:
            raise ValueError(
                f"texture atlas must contain {TEXTURE_BYTES} bytes, got {len(textures)}"
            )
        self.textures = textures

    def clear(self) -> None:
        self.back_buffer[:] = self._cleared_frame
        self.z_buffer[:] = self._cleared_depth
        self.vsync_latch = 0

    def set_palette_entry(self, index: int, r: int, g: int, b: int) -> None:
        self.palette[index] = (r, g, b)
        self.palette_dirty = True

    def submit_quads(self, quads: Iterable[QuadDesc]) -> None:
        for quad in quads:
            self._rasterize_quad(quad)

    def flip(self) -> None:
        self.front_buffer, self.back_buffer = self.back_buffer, self.front_buffer
        self.frame_count += 1
        self.vsync_latch = 1

    def status_tuple(self) -> tuple[int, int, int, int, int, int]:
        busy = 0
        fifo_full = 0
        fifo_empty = 1
        raw = (self.vsync_latch << 3) | (fifo_empty << 2) | (fifo_full << 1) | busy
        return raw, 0, busy, fifo_full, fifo_empty, self.vsync_latch

    def _rasterize_quad(self, quad: QuadDesc) -> None:
        x_min = max(0, quad.x_min)
        y_min = max(0, quad.y_min)
        x_max = min(self.width - 1, quad.x_max)
        y_max = min(self.height - 1, quad.y_max)

        if x_min > x_max or y_min > y_max:
            return

        width = self.width
        qx_min = quad.x_min
        qy_min = quad.y_min
        z0 = quad.z0
        dz_dx = quad.dz_dx
        dz_dy = quad.dz_dy
        edges = quad.edges
        textured = (quad.flags & QUAD_FLAG_TEX) != 0
        ztest = (quad.flags & QUAD_FLAG_ZTEST) != 0
        alpha_key = (quad.flags & QUAD_FLAG_ALPHA_KEY) != 0

        if textured:
            if quad.uv is None:
                raise ValueError("textured quad is missing a UV block")
            u0, v0, du_dx, dv_dx, du_dy, dv_dy = quad.uv
            tile_offset = (quad.tex_or_color & 0x3F) << 8
        else:
            color = quad.tex_or_color

        for y in range(y_min, y_max + 1):
            row_base = y * width
            dy = y - qy_min
            for x in range(x_min, x_max + 1):
                inside = True
                for a_coef, b_coef, c_coef in edges:
                    if a_coef * x + b_coef * y + c_coef < 0:
                        inside = False
                        break

                if not inside:
                    continue

                z_value = z0 + dz_dx * (x - qx_min) + dz_dy * dy
                if z_value < 0:
                    z_value = 0
                elif z_value > CLEAR_DEPTH:
                    z_value = CLEAR_DEPTH

                if textured:
                    dx = x - qx_min
                    u_value = u0 + du_dx * dx + du_dy * dy
                    v_value = v0 + dv_dx * dx + dv_dy * dy
                    tex_u = (u_value >> 16) & 0xF
                    tex_v = (v_value >> 16) & 0xF
                    color = self.textures[tile_offset | (tex_v << 4) | tex_u]
                    if alpha_key and color == 0:
                        continue

                pixel_index = row_base + x
                if ztest:
                    if z_value >= self.z_buffer[pixel_index]:
                        continue
                    self.z_buffer[pixel_index] = z_value

                self.back_buffer[pixel_index] = color
