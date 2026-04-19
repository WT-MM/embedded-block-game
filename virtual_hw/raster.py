from __future__ import annotations

from array import array
from typing import Iterable, List, Sequence, Tuple

from protocol import QUAD_FLAG_ZTEST, QuadDesc


class VirtualGPU:
    def __init__(self, width: int = 320, height: int = 240) -> None:
        self.width = width
        self.height = height
        self.pixel_count = width * height
        self.front_buffer = bytearray(self.pixel_count)
        self.back_buffer = bytearray(self.pixel_count)
        self.z_buffer = array("H", [0xFFFF]) * self.pixel_count
        self.palette: List[Tuple[int, int, int]] = [(0, 0, 0)] * 256
        self.frame_count = 0
        self.vsync_latch = 0
        self.palette_dirty = True

    def clear(self) -> None:
        self.back_buffer[:] = b"\x00" * self.pixel_count
        self.z_buffer[:] = array("H", [0xFFFF]) * self.pixel_count
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
        ztest = (quad.flags & QUAD_FLAG_ZTEST) != 0

        if x_min > x_max or y_min > y_max:
            return

        for y in range(y_min, y_max + 1):
            row_base = y * self.width
            dy = y - quad.y_min
            for x in range(x_min, x_max + 1):
                inside = True
                for a_coef, b_coef, c_coef in quad.edges:
                    if a_coef * x + b_coef * y + c_coef < 0:
                        inside = False
                        break

                if not inside:
                    continue

                dx = x - quad.x_min
                z_value = quad.z0 + quad.dz_dx * dx + quad.dz_dy * dy
                if z_value < 0:
                    z_value = 0
                elif z_value > 0xFFFF:
                    z_value = 0xFFFF

                pixel_index = row_base + x
                if ztest:
                    if z_value >= self.z_buffer[pixel_index]:
                        continue
                    self.z_buffer[pixel_index] = z_value

                self.back_buffer[pixel_index] = quad.tex_or_color
