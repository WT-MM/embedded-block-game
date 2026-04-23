from __future__ import annotations

from array import array
from collections.abc import Iterable
from pathlib import Path

from .protocol import (
    QUAD_FLAG_ALPHA_KEY,
    QUAD_FLAG_FOG,
    QUAD_FLAG_TEX,
    QUAD_FLAG_ZTEST,
    QUAD_ALPHA_MASK,
    QUAD_ALPHA_SHIFT,
    QUAD_LIGHT_MASK,
    QUAD_LIGHT_SHIFT,
    QuadDesc,
)

CLEAR_DEPTH = 0xFFFF
TEXTURE_TILE_SIZE = 16
TEXTURE_TILE_COUNT = 64
TEXTURE_BYTES = TEXTURE_TILE_COUNT * TEXTURE_TILE_SIZE * TEXTURE_TILE_SIZE
RECIP_LUT_SIZE = 1025
RECIP_LUT_STEP = 64
Q16 = 1 << 16
Q32 = 1 << 32
I32_MIN = -(1 << 31)
I32_MAX = (1 << 31) - 1
U32_MASK = 0xFFFF_FFFF
TEXTURE_COORD_MAX_Q32 = TEXTURE_TILE_SIZE << 32

RECIP_LUT = tuple(
    round(Q32 / (Q16 + index * RECIP_LUT_STEP))
    for index in range(RECIP_LUT_SIZE)
)


def apply_light_bank(color: int, flags: int) -> int:
    if color == 0:
        return 0

    bank = (flags & QUAD_LIGHT_MASK) >> QUAD_LIGHT_SHIFT
    if bank == 0 or color >= 64:
        return color

    return (bank << 6) | color


def rgb888_to_rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb888(value: int) -> tuple[int, int, int]:
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def blend_rgb565(src: int, dst: int, alpha: int) -> int:
    src_r = (src >> 11) & 0x1F
    src_g = (src >> 5) & 0x3F
    src_b = src & 0x1F
    dst_r = (dst >> 11) & 0x1F
    dst_g = (dst >> 5) & 0x3F
    dst_b = dst & 0x1F

    if alpha == 1:
        out_r = ((src_r * 3) + dst_r + 2) >> 2
        out_g = ((src_g * 3) + dst_g + 2) >> 2
        out_b = ((src_b * 3) + dst_b + 2) >> 2
    elif alpha == 2:
        out_r = (src_r + dst_r + 1) >> 1
        out_g = (src_g + dst_g + 1) >> 1
        out_b = (src_b + dst_b + 1) >> 1
    elif alpha == 3:
        out_r = (src_r + (dst_r * 3) + 2) >> 2
        out_g = (src_g + (dst_g * 3) + 2) >> 2
        out_b = (src_b + (dst_b * 3) + 2) >> 2
    else:
        return src & 0xFFFF

    return ((out_r & 0x1F) << 11) | ((out_g & 0x3F) << 5) | (out_b & 0x1F)


def _clamp_x(value: int) -> int:
    if value < 0:
        return 0
    if value > 319:
        return 319
    return value


def _clamp_y(value: int) -> int:
    if value < 0:
        return 0
    if value > 239:
        return 239
    return value


def _clamp_z(value: int) -> int:
    if value < 0:
        return 0
    if value > CLEAR_DEPTH:
        return CLEAR_DEPTH
    return value


def _clamp_s32(value: int) -> int:
    if value > I32_MAX:
        return I32_MAX
    if value < I32_MIN:
        return I32_MIN
    return value


def _clamp_pos_u32(value: int) -> int:
    if value <= 0:
        return 0
    if value > I32_MAX:
        return I32_MAX
    return value


def _to_signed32(value: int) -> int:
    value &= U32_MASK
    if value & (1 << 31):
        return value - (1 << 32)
    return value


def _msb_index32(value: int) -> int:
    if value == 0:
        return 0
    return value.bit_length() - 1


def _reciprocal_q16_16(iw_q: int) -> int:
    """Return the FPGA reciprocal-unit result for Q16.16 1/w."""

    iw_q &= U32_MASK
    if iw_q == 0:
        return 0

    iw_msb = _msb_index32(iw_q)
    if iw_msb >= 16:
        iw_norm_q = iw_q >> (iw_msb - 16)
    else:
        iw_norm_q = (iw_q << (16 - iw_msb)) & U32_MASK

    phase = iw_norm_q & 0xFFFF
    lut_idx = phase >> 6
    lut_frac = phase & 0x3F
    w_norm_lo = RECIP_LUT[lut_idx]
    w_norm_hi = RECIP_LUT[lut_idx + 1]
    interp_step = ((w_norm_lo - w_norm_hi) * lut_frac + 32) >> 6
    w_norm_q = (w_norm_lo - interp_step) & U32_MASK

    if iw_msb >= 16:
        return (w_norm_q >> (iw_msb - 16)) & U32_MASK
    return (w_norm_q << (16 - iw_msb)) & U32_MASK


def _texture_coord_from_product(value: int) -> int:
    if value <= 0:
        return 0
    if value >= TEXTURE_COORD_MAX_Q32:
        return TEXTURE_TILE_SIZE - 1
    return (value >> 32) & 0xF


def _apply_fog_rgb565(
    src_rgb565: int,
    fog_rgb565: int,
    flags: int,
    fog_enabled: bool,
    fog_start: int,
    fog_end: int,
    fog_inv_proj_sq: int,
    w_q16_16: int,
    x: int,
    y: int,
) -> int:
    if (
        not fog_enabled
        or not (flags & QUAD_FLAG_FOG)
        or fog_end <= fog_start
        or w_q16_16 <= 0
    ):
        return src_rgb565

    dx = x - 160
    dy = 120 - y
    radial_sq = dx * dx + dy * dy
    radial_sq_q16 = (radial_sq * fog_inv_proj_sq) & U32_MASK
    ray_scale_q16 = 65536 + ((radial_sq_q16 * 3) >> 3)
    radial_q8_8 = ((w_q16_16 * ray_scale_q16) >> 24) & 0xFFFF

    if radial_q8_8 <= fog_start:
        return src_rgb565
    if radial_q8_8 >= fog_end:
        return fog_rgb565

    span = fog_end - fog_start
    q1 = fog_start + (span >> 2)
    q2 = fog_start + (span >> 1)

    if radial_q8_8 < q1:
        fog_alpha = 1
    elif radial_q8_8 < q2:
        fog_alpha = 2
    else:
        fog_alpha = 3

    return blend_rgb565(src_rgb565, fog_rgb565, fog_alpha)


def load_texture_mif(path: str | Path) -> bytes:
    """Parse an Altera Memory Initialization File.

    This is the same file Quartus feeds to altsyncram's `init_file` for
    the texture ROM in `voxel_gpu.sv`. Keeping one source of truth for
    synthesis and simulation avoids drift -- see `hw/generate_textures.py`
    for how it is produced.

    The MIF grammar we need to support is tiny:

        WIDTH  = 8;
        DEPTH  = 16384;
        ADDRESS_RADIX = HEX;
        DATA_RADIX    = HEX;
        CONTENT BEGIN
            0000 : 01;
            0001 : 02;
            [A:B]  : FF;          -- range fill (A..B inclusive)
            [A..B] : FF;          -- alt. range syntax
            END;

    Anything from `--` to end-of-line is a comment. We treat unspecified
    addresses as 0, matching Quartus's behaviour.
    """
    texture_path = Path(path)
    data = [0] * TEXTURE_BYTES

    width = None
    depth = None
    addr_radix = 16
    data_radix = 16
    in_content = False

    def _strip_comment(s: str) -> str:
        idx = s.find("--")
        return s[:idx] if idx >= 0 else s

    def _parse_int(tok: str, radix: int, line_no: int) -> int:
        try:
            return int(tok, radix)
        except ValueError as exc:
            raise ValueError(
                f"{texture_path}:{line_no}: bad {radix}-radix integer {tok!r}"
            ) from exc

    with texture_path.open("r", encoding="ascii") as handle:
        for line_no, raw_line in enumerate(handle, start=1):
            line = _strip_comment(raw_line).strip()
            if not line:
                continue

            upper = line.upper()
            if upper.startswith("WIDTH"):
                width = int(line.split("=", 1)[1].rstrip(";").strip(), 10)
                continue
            if upper.startswith("DEPTH"):
                depth = int(line.split("=", 1)[1].rstrip(";").strip(), 10)
                continue
            if upper.startswith("ADDRESS_RADIX"):
                rv = line.split("=", 1)[1].rstrip(";").strip().upper()
                addr_radix = {"HEX": 16, "DEC": 10, "BIN": 2, "OCT": 8}[rv]
                continue
            if upper.startswith("DATA_RADIX"):
                rv = line.split("=", 1)[1].rstrip(";").strip().upper()
                data_radix = {"HEX": 16, "DEC": 10, "BIN": 2, "OCT": 8}[rv]
                continue
            if upper.startswith("CONTENT"):
                in_content = True
                continue
            if upper.startswith("BEGIN"):
                in_content = True
                continue
            if upper.startswith("END"):
                in_content = False
                continue

            if not in_content:
                continue

            stmt = line.rstrip(";").strip()
            if ":" not in stmt:
                raise ValueError(f"{texture_path}:{line_no}: expected ':' in {line!r}")
            lhs, rhs = stmt.split(":", 1)
            lhs = lhs.strip()
            rhs = rhs.strip()
            value = _parse_int(rhs, data_radix, line_no)

            if lhs.startswith("[") and lhs.endswith("]"):
                inner = lhs[1:-1]
                sep = ".." if ".." in inner else ":"
                lo_tok, hi_tok = inner.split(sep, 1)
                lo = _parse_int(lo_tok.strip(), addr_radix, line_no)
                hi = _parse_int(hi_tok.strip(), addr_radix, line_no)
                for addr in range(lo, hi + 1):
                    if 0 <= addr < TEXTURE_BYTES:
                        data[addr] = value & 0xFF
            else:
                addr = _parse_int(lhs, addr_radix, line_no)
                if 0 <= addr < TEXTURE_BYTES:
                    data[addr] = value & 0xFF

    if width is not None and width != 8:
        raise ValueError(f"{texture_path}: expected WIDTH=8, got {width}")
    if depth is not None and depth != TEXTURE_BYTES:
        raise ValueError(
            f"{texture_path}: expected DEPTH={TEXTURE_BYTES}, got {depth}"
        )

    return bytes(data)


class VirtualGPU:
    def __init__(
        self, width: int = 320, height: int = 240, textures: bytes | None = None
    ) -> None:
        self.width = width
        self.height = height
        self.pixel_count = width * height
        # Framebuffers hold resolved RGB565 pixels to match the real HW's
        # translucent draw path.
        self.front_buffer = array("H", [0]) * self.pixel_count
        self.back_buffer = array("H", [0]) * self.pixel_count
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
        # The real HW's ST_CLEAR sweep resolves palette index 0 into the
        # RGB565 framebuffer.
        clear_rgb = rgb888_to_rgb565(self.palette[0])
        self.back_buffer[:] = array("H", [clear_rgb]) * self.pixel_count
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

    def submit_quads(self, quads: Iterable[QuadDesc]) -> int:
        count = 0
        for quad in quads:
            self._rasterize_quad(quad)
            count += 1
        return count

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
        fb_x_limit = min(self.width - 1, 319)
        fb_y_limit = min(self.height - 1, 239)
        x_min = min(_clamp_x(quad.x_min), fb_x_limit)
        y_min = min(_clamp_y(quad.y_min), fb_y_limit)
        x_max = min(_clamp_x(quad.x_max), fb_x_limit)
        y_max = min(_clamp_y(quad.y_max), fb_y_limit)

        if x_min > x_max or y_min > y_max:
            return

        width = self.width
        qx_min = x_min
        qy_min = y_min
        z0 = quad.z0
        dz_dx = quad.dz_dx
        dz_dy = quad.dz_dy
        edges = quad.edges
        textured = (quad.flags & QUAD_FLAG_TEX) != 0
        ztest = (quad.flags & QUAD_FLAG_ZTEST) != 0
        alpha_key = (quad.flags & QUAD_FLAG_ALPHA_KEY) != 0
        alpha = (quad.flags & QUAD_ALPHA_MASK) >> QUAD_ALPHA_SHIFT

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

                dx = x - qx_min
                z_value = _clamp_z(z0 + dz_dx * dx + dz_dy * dy)

                w_q16_16 = 0
                if textured:
                    uw = _clamp_s32(
                        u_over_w_0 + u_over_w_dx * dx + u_over_w_dy * dy
                    )
                    vw = _clamp_s32(
                        v_over_w_0 + v_over_w_dx * dx + v_over_w_dy * dy
                    )
                    iw = _clamp_pos_u32(
                        one_over_w_0 + one_over_w_dx * dx + one_over_w_dy * dy
                    )
                    w_q16_16 = _reciprocal_q16_16(iw)
                    tex_u = _texture_coord_from_product(
                        _to_signed32(uw) * _to_signed32(w_q16_16)
                    )
                    tex_v = _texture_coord_from_product(
                        _to_signed32(vw) * _to_signed32(w_q16_16)
                    )
                    raw_color = self.textures[tile_offset | (tex_v << 4) | tex_u]
                    if alpha_key and raw_color == 0:
                        continue
                    color_index = apply_light_bank(raw_color, quad.flags)

                pixel_index = row_base + x
                if ztest:
                    if z_value >= self.z_buffer[pixel_index]:
                        continue
                    self.z_buffer[pixel_index] = z_value

                src_rgb565 = rgb888_to_rgb565(self.palette[color_index])
                src_rgb565 = _apply_fog_rgb565(
                    src_rgb565,
                    rgb888_to_rgb565(self.palette[self.fog_color]),
                    quad.flags,
                    self.fog_enabled,
                    self.fog_start,
                    self.fog_end,
                    self.fog_inv_proj_sq,
                    w_q16_16,
                    x,
                    y,
                )
                dst_rgb565 = self.back_buffer[pixel_index]
                self.back_buffer[pixel_index] = blend_rgb565(
                    src_rgb565, dst_rgb565, alpha
                )
