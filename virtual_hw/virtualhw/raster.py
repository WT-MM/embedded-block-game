from __future__ import annotations

from array import array
from collections.abc import Iterable
from pathlib import Path

from .protocol import (
    QUAD_FLAG_ALPHA_KEY,
    QUAD_FLAG_FOG,
    QUAD_FLAG_TEX,
    QUAD_FLAG_ZTEST,
    QUAD_LIGHT_MASK,
    QUAD_LIGHT_SHIFT,
    QuadDesc,
)

CLEAR_DEPTH = 0xFFFF
TEXTURE_TILE_SIZE = 16
TEXTURE_TILE_COUNT = 64
TEXTURE_BYTES = TEXTURE_TILE_COUNT * TEXTURE_TILE_SIZE * TEXTURE_TILE_SIZE

# Standard 4x4 ordered-dither Bayer matrix, values 0..15. Matches the
# bayer4() function in hw/voxel_gpu.sv exactly so the virtual output is
# visually interchangeable with the real GPU.
BAYER4 = (
    ( 0,  8,  2, 10),
    (12,  4, 14,  6),
    ( 3, 11,  1,  9),
    (15,  7, 13,  5),
)


def apply_light_bank(color: int, flags: int) -> int:
    if color == 0:
        return 0

    bank = (flags & QUAD_LIGHT_MASK) >> QUAD_LIGHT_SHIFT
    if bank == 0 or color >= 64:
        return color

    return (bank << 6) | color


def _fog_replace(
    flags: int,
    fog_enabled: bool,
    fog_start: int,
    fog_end: int,
    fog_inv_proj_sq: int,
    forward_z_q16_16: int,
    x: int,
    y: int,
    width: int,
    height: int,
) -> bool:
    """Return True if the pixel should be replaced by the fog color.

    Mirrors the hardware path in hw/voxel_gpu.sv: we estimate radial
    distance from per-pixel w by scaling with sqrt(1 + r^2/f^2) (linearly
    approximated as 1 + 3/8 * r^2/f^2), bucket it against the quartile
    thresholds derived from fog_start/fog_end, then gate that threshold
    with an ordered 4x4 Bayer dither.
    """

    if (
        not fog_enabled
        or not (flags & QUAD_FLAG_FOG)
        or fog_end <= fog_start
        or forward_z_q16_16 <= 0
    ):
        return False

    dx = x - (width // 2)
    dy = (height // 2) - y
    radial_sq = dx * dx + dy * dy
    radial_sq_q16 = radial_sq * fog_inv_proj_sq
    ray_scale_q16 = 65536 + ((radial_sq_q16 * 3) >> 3)
    radial_q8_8 = ((forward_z_q16_16 * ray_scale_q16) >> 16) >> 8
    radial_q8_8 &= 0xFFFF

    if radial_q8_8 <= fog_start:
        return False
    if radial_q8_8 >= fog_end:
        return True

    span = fog_end - fog_start
    q1 = fog_start + (span >> 2)
    q2 = fog_start + (span >> 1)
    q3 = fog_end - (span >> 2)

    if radial_q8_8 < q1:
        threshold = 4
    elif radial_q8_8 < q2:
        threshold = 8
    elif radial_q8_8 < q3:
        threshold = 12
    else:
        threshold = 15

    return BAYER4[y & 3][x & 3] < threshold


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
        # Framebuffers hold 8-bit palette indices to match the real HW's
        # per-pixel storage; the monitor converts to RGB at scanout time by
        # looking up the palette.
        self.front_buffer = array("B", [0]) * self.pixel_count
        self.back_buffer = array("B", [0]) * self.pixel_count
        self._cleared_depth = array("H", [CLEAR_DEPTH]) * self.pixel_count
        self.z_buffer = self._cleared_depth[:]
        self.palette: list[tuple[int, int, int]] = [(0, 0, 0)] * 256
        self.frame_count = 0
        self.vsync_latch = 0
        self.fog_start = 0
        self.fog_end = 0
        self.fog_color = 0
        self.fog_enabled = False
        self.fog_inv_proj_sq = 0
        if textures is None:
            textures = bytes(TEXTURE_BYTES)
        if len(textures) != TEXTURE_BYTES:
            raise ValueError(
                f"texture atlas must contain {TEXTURE_BYTES} bytes, got {len(textures)}"
            )
        self.textures = textures

    def clear(self) -> None:
        # The real HW's ST_CLEAR sweep writes palette index 0 directly
        # into the 8-bit framebuffer; we mirror that here.
        self.back_buffer[:] = array("B", [0]) * self.pixel_count
        self.z_buffer[:] = self._cleared_depth
        self.vsync_latch = 0

    def set_palette_entry(self, index: int, r: int, g: int, b: int) -> None:
        self.palette[index] = (r, g, b)

    def set_fog(
        self,
        start_dist: int,
        end_dist: int,
        color_index: int,
        enabled: int,
        inv_proj_sq: int,
    ) -> None:
        self.fog_start = start_dist & 0xFFFF
        self.fog_end = end_dist & 0xFFFF
        self.fog_color = color_index & 0xFF
        self.fog_enabled = bool(enabled)
        self.fog_inv_proj_sq = inv_proj_sq & 0xFFFF

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
        # Real HW ignores the QUAD_ALPHA_* bits; there is no blend path.

        if textured:
            if quad.uv is None:
                raise ValueError("textured quad is missing a UV block")
            (
                u_over_w_0,
                u_over_w_dx,
                u_over_w_dy,
                v_over_w_0,
                v_over_w_dx,
                v_over_w_dy,
                one_over_w_0,
                one_over_w_dx,
                one_over_w_dy,
            ) = quad.uv
            tile_offset = (quad.tex_or_color & 0x3F) << 8
        else:
            color_index = apply_light_bank(quad.tex_or_color, quad.flags)

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

                forward_z_q16_16 = 0
                if textured:
                    dx = x - qx_min
                    uw = u_over_w_0 + u_over_w_dx * dx + u_over_w_dy * dy
                    vw = v_over_w_0 + v_over_w_dx * dx + v_over_w_dy * dy
                    iw = one_over_w_0 + one_over_w_dx * dx + one_over_w_dy * dy
                    if iw <= 0:
                        continue
                    forward_z_q16_16 = (1 << 32) // iw
                    u_value = (uw * forward_z_q16_16) >> 32
                    v_value = (vw * forward_z_q16_16) >> 32
                    tex_u = u_value & 0xF
                    tex_v = v_value & 0xF
                    raw_color = self.textures[tile_offset | (tex_v << 4) | tex_u]
                    if alpha_key and raw_color == 0:
                        continue
                    color_index = apply_light_bank(raw_color, quad.flags)

                pixel_index = row_base + x
                if ztest:
                    if z_value >= self.z_buffer[pixel_index]:
                        continue
                    self.z_buffer[pixel_index] = z_value

                if _fog_replace(
                    quad.flags,
                    self.fog_enabled,
                    self.fog_start,
                    self.fog_end,
                    self.fog_inv_proj_sq,
                    forward_z_q16_16,
                    x,
                    y,
                    self.width,
                    self.height,
                ):
                    out_index = self.fog_color
                else:
                    out_index = color_index

                self.back_buffer[pixel_index] = out_index & 0xFF
