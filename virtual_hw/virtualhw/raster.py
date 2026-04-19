from __future__ import annotations

from array import array
from collections.abc import Iterable

from .protocol import QUAD_FLAG_ZTEST, QuadDesc

CLEAR_DEPTH = 0xFFFF


class VirtualGPU:
    def __init__(self, width: int = 320, height: int = 240) -> None:
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
        color = quad.tex_or_color
        edges = quad.edges
        ztest = (quad.flags & QUAD_FLAG_ZTEST) != 0

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

                pixel_index = row_base + x
                if ztest:
                    if z_value >= self.z_buffer[pixel_index]:
                        continue
                    self.z_buffer[pixel_index] = z_value

                self.back_buffer[pixel_index] = color
