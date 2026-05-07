from __future__ import annotations

import os
from collections.abc import Iterable
from pathlib import Path

import numpy as np

from .protocol import (
    QUAD_FLAG_ALPHA_KEY,
    QUAD_FLAG_FOG,
    QUAD_FLAG_TEX,
    QUAD_FLAG_ZTEST,
    QUAD_ALPHA_MASK,
    QUAD_ALPHA_SHIFT,
    QUAD_LIGHT_MASK,
    QUAD_LIGHT_SHIFT,
    TEX_REPEAT_UV,
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
RECIP_LUT_NP = np.asarray(RECIP_LUT, dtype=np.int64)

# JIT dispatch. Set VIRTUALHW_JIT=0 to force the pure-Python path.
_JIT_REQUEST = os.environ.get("VIRTUALHW_JIT", "1") != "0"
try:
    if _JIT_REQUEST:
        from numba import njit as _njit  # type: ignore

        JIT_ENABLED = True
    else:  # pragma: no cover - explicit opt-out
        JIT_ENABLED = False
except ImportError:  # pragma: no cover - numba missing
    JIT_ENABLED = False

if JIT_ENABLED:
    _kernel_jit = _njit(cache=True)
else:
    def _kernel_jit(fn):
        return fn


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


# ---------------------------------------------------------------------------
# Numba-compatible kernel. Helpers below are intentionally written in a subset
# of Python that Numba can compile: no dataclasses, no tuples as return values
# containing mixed types, no str / bit_length() calls, only primitives and
# numpy arrays.
# ---------------------------------------------------------------------------


@_kernel_jit
def _k_apply_light_bank(color: int, flags: int) -> int:
    if color == 0:
        return 0
    bank = (flags & 0x30) >> 4  # QUAD_LIGHT_MASK >> QUAD_LIGHT_SHIFT
    if bank == 0 or color >= 64:
        return color
    return (bank << 6) | color


@_kernel_jit
def _k_blend_rgb565(src: int, dst: int, alpha: int) -> int:
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


@_kernel_jit
def _k_msb_index32(value: int) -> int:
    # Equivalent to value.bit_length() - 1 for nonzero inputs, which Numba
    # does not support. Loop bound is at most 32.
    if value == 0:
        return 0
    idx = 0
    v = value
    while v > 1:
        v >>= 1
        idx += 1
    return idx


@_kernel_jit
def _k_reciprocal_q16_16(iw_q: int, recip_lut) -> int:
    iw_q &= 0xFFFF_FFFF
    if iw_q == 0:
        return 0

    iw_msb = _k_msb_index32(iw_q)
    if iw_msb >= 16:
        iw_norm_q = iw_q >> (iw_msb - 16)
    else:
        iw_norm_q = (iw_q << (16 - iw_msb)) & 0xFFFF_FFFF

    phase = iw_norm_q & 0xFFFF
    lut_idx = phase >> 6
    lut_frac = phase & 0x3F
    w_norm_lo = recip_lut[lut_idx]
    w_norm_hi = recip_lut[lut_idx + 1]
    interp_step = ((w_norm_lo - w_norm_hi) * lut_frac + 32) >> 6
    w_norm_q = (w_norm_lo - interp_step) & 0xFFFF_FFFF

    if iw_msb >= 16:
        return (w_norm_q >> (iw_msb - 16)) & 0xFFFF_FFFF
    return (w_norm_q << (16 - iw_msb)) & 0xFFFF_FFFF


@_kernel_jit
def _k_to_signed32(value: int) -> int:
    value &= 0xFFFF_FFFF
    if value & 0x8000_0000:
        return value - 0x1_0000_0000
    return value


@_kernel_jit
def _k_texture_coord(value: int, repeat: int) -> int:
    if value <= 0:
        return 0
    if repeat != 0:
        return (value >> 32) & 0xF
    if value >= (16 << 32):
        return 15
    return (value >> 32) & 0xF


@_kernel_jit
def _k_apply_fog_rgb565(
    src_rgb565: int,
    fog_rgb565: int,
    flags: int,
    fog_enabled: int,
    fog_start: int,
    fog_end: int,
    fog_inv_proj_sq: int,
    w_q16_16: int,
    x: int,
    y: int,
) -> int:
    if fog_enabled == 0 or (flags & 0x8) == 0 or fog_end <= fog_start or w_q16_16 <= 0:
        return src_rgb565

    dx = x - 320
    dy = 240 - y
    radial_sq = dx * dx + dy * dy
    radial_sq_q16 = (radial_sq * fog_inv_proj_sq) & 0xFFFF_FFFF
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

    return _k_blend_rgb565(src_rgb565, fog_rgb565, fog_alpha)


@_kernel_jit
def _k_rasterize_quad(
    back_buffer,
    z_buffer,
    width: int,
    height: int,
    palette_rgb565,
    textures,
    recip_lut,
    fog_enabled: int,
    fog_start: int,
    fog_end: int,
    fog_color_rgb565: int,
    fog_inv_proj_sq: int,
    x_min: int,
    y_min: int,
    x_max: int,
    y_max: int,
    edges,  # shape (4, 3), int64
    z0: int,
    dz_dx: int,
    dz_dy: int,
    tex_or_color: int,
    flags: int,
    has_uv: int,
    uv,  # shape (9,), int64; ignored when has_uv == 0
) -> None:
    fb_x_limit = width - 1
    fb_y_limit = height - 1

    if x_min < 0:
        x_min = 0
    elif x_min > fb_x_limit:
        x_min = fb_x_limit
    if y_min < 0:
        y_min = 0
    elif y_min > fb_y_limit:
        y_min = fb_y_limit
    if x_max < 0:
        x_max = 0
    elif x_max > fb_x_limit:
        x_max = fb_x_limit
    if y_max < 0:
        y_max = 0
    elif y_max > fb_y_limit:
        y_max = fb_y_limit

    if x_min > x_max or y_min > y_max:
        return

    textured = 1 if (flags & 0x1) != 0 else 0
    ztest = 1 if (flags & 0x2) != 0 else 0
    alpha_key = 1 if (flags & 0x4) != 0 else 0
    fog_flag = 1 if (flags & 0x8) != 0 else 0
    alpha = (flags & 0xC0) >> 6

    a0 = edges[0, 0]; b0 = edges[0, 1]; c0 = edges[0, 2]
    a1 = edges[1, 0]; b1 = edges[1, 1]; c1 = edges[1, 2]
    a2 = edges[2, 0]; b2 = edges[2, 1]; c2 = edges[2, 2]
    a3 = edges[3, 0]; b3 = edges[3, 1]; c3 = edges[3, 2]

    # Opaque, non-textured, no ztest, no fog, edges cover bbox -> scanline fill.
    if textured == 0 and ztest == 0 and alpha == 0 and fog_flag == 0:
        covers = 1
        for (xx, yy) in ((x_min, y_min), (x_max, y_min), (x_min, y_max), (x_max, y_max)):
            if a0 * xx + b0 * yy + c0 < 0:
                covers = 0
            elif a1 * xx + b1 * yy + c1 < 0:
                covers = 0
            elif a2 * xx + b2 * yy + c2 < 0:
                covers = 0
            elif a3 * xx + b3 * yy + c3 < 0:
                covers = 0
        if covers == 1:
            color_index = _k_apply_light_bank(tex_or_color, flags)
            src_rgb565 = palette_rgb565[color_index]
            for y in range(y_min, y_max + 1):
                row_start = y * width
                for x in range(x_min, x_max + 1):
                    back_buffer[row_start + x] = src_rgb565
            return

    if textured != 0:
        u_over_w_0 = uv[0]
        u_over_w_dx = uv[1]
        u_over_w_dy = uv[2]
        v_over_w_0 = uv[3]
        v_over_w_dx = uv[4]
        v_over_w_dy = uv[5]
        one_over_w_0 = uv[6]
        one_over_w_dx = uv[7]
        one_over_w_dy = uv[8]
        tile_offset = (tex_or_color & 0x3F) << 8
        repeat_uv = 1 if (tex_or_color & 0x40) != 0 else 0
    else:
        u_over_w_0 = 0
        u_over_w_dx = 0
        u_over_w_dy = 0
        v_over_w_0 = 0
        v_over_w_dx = 0
        v_over_w_dy = 0
        one_over_w_0 = 0
        one_over_w_dx = 0
        one_over_w_dy = 0
        tile_offset = 0
        repeat_uv = 0
        color_index_flat = _k_apply_light_bank(tex_or_color, flags)

    e0_row = a0 * x_min + b0 * y_min + c0
    e1_row = a1 * x_min + b1 * y_min + c1
    e2_row = a2 * x_min + b2 * y_min + c2
    e3_row = a3 * x_min + b3 * y_min + c3
    z_row = z0
    uw_row = u_over_w_0
    vw_row = v_over_w_0
    iw_row = one_over_w_0

    for y in range(y_min, y_max + 1):
        row_base = y * width
        e0 = e0_row
        e1 = e1_row
        e2 = e2_row
        e3 = e3_row
        z_raw = z_row
        uw_raw = uw_row
        vw_raw = vw_row
        iw_raw = iw_row

        for x in range(x_min, x_max + 1):
            if e0 >= 0 and e1 >= 0 and e2 >= 0 and e3 >= 0:
                pixel_index = row_base + x
                z_value = z_raw
                if z_value < 0:
                    z_value = 0
                elif z_value > 0xFFFF:
                    z_value = 0xFFFF

                if ztest == 0 or z_value < z_buffer[pixel_index]:
                    w_q16_16 = 0
                    transparent = 0
                    if textured != 0:
                        uw = uw_raw
                        if uw > 0x7FFF_FFFF:
                            uw = 0x7FFF_FFFF
                        elif uw < -0x8000_0000:
                            uw = -0x8000_0000
                        vw = vw_raw
                        if vw > 0x7FFF_FFFF:
                            vw = 0x7FFF_FFFF
                        elif vw < -0x8000_0000:
                            vw = -0x8000_0000
                        iw = iw_raw
                        if iw <= 0:
                            iw = 0
                        elif iw > 0x7FFF_FFFF:
                            iw = 0x7FFF_FFFF
                        w_q16_16 = _k_reciprocal_q16_16(iw, recip_lut)
                        tex_u = _k_texture_coord(
                            _k_to_signed32(uw) * _k_to_signed32(w_q16_16),
                            repeat_uv,
                        )
                        tex_v = _k_texture_coord(
                            _k_to_signed32(vw) * _k_to_signed32(w_q16_16),
                            repeat_uv,
                        )
                        raw_color = textures[tile_offset | (tex_v << 4) | tex_u]
                        if alpha_key != 0 and raw_color == 0:
                            transparent = 1
                        else:
                            color_index = _k_apply_light_bank(raw_color, flags)
                    else:
                        color_index = color_index_flat

                    if transparent == 0:
                        if ztest != 0:
                            z_buffer[pixel_index] = z_value

                        src_rgb565 = palette_rgb565[color_index]
                        src_rgb565 = _k_apply_fog_rgb565(
                            src_rgb565,
                            fog_color_rgb565,
                            flags,
                            fog_enabled,
                            fog_start,
                            fog_end,
                            fog_inv_proj_sq,
                            w_q16_16,
                            x,
                            y,
                        )
                        dst_rgb565 = back_buffer[pixel_index]
                        back_buffer[pixel_index] = _k_blend_rgb565(
                            src_rgb565, dst_rgb565, alpha
                        )

            e0 += a0
            e1 += a1
            e2 += a2
            e3 += a3
            z_raw += dz_dx
            uw_raw += u_over_w_dx
            vw_raw += v_over_w_dx
            iw_raw += one_over_w_dx

        e0_row += b0
        e1_row += b1
        e2_row += b2
        e3_row += b3
        z_row += dz_dy
        uw_row += u_over_w_dy
        vw_row += v_over_w_dy
        iw_row += one_over_w_dy


def load_texture_mif(path: str | Path) -> bytes:
    """Parse an Altera Memory Initialization File.

    This is the same file Quartus feeds to altsyncram's `init_file` for
    the texture ROM in `hw/voxel_gpu/rtl/voxel_gpu.sv`. Keeping one source of truth for
    synthesis and simulation avoids drift -- see
    `hw/voxel_gpu/scripts/generate_textures.py`
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


_EMPTY_UV_NP = np.zeros(9, dtype=np.int64)


class VirtualGPU:
    def __init__(
        self, width: int = 640, height: int = 480, textures: bytes | None = None
    ) -> None:
        self.width = width
        self.height = height
        self.pixel_count = width * height
        # Framebuffers hold resolved RGB565 pixels to match the real HW's
        # translucent draw path.
        self.front_buffer = np.zeros(self.pixel_count, dtype=np.uint16)
        self.back_buffer = np.zeros(self.pixel_count, dtype=np.uint16)
        self.z_buffer = np.full(self.pixel_count, CLEAR_DEPTH, dtype=np.uint16)
        self.palette: list[tuple[int, int, int]] = [(0, 0, 0)] * 256
        self.palette_rgb565 = np.zeros(256, dtype=np.uint16)
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
        self._textures_np = np.frombuffer(textures, dtype=np.uint8)

    def clear(self) -> None:
        # The real HW's ST_CLEAR sweep resolves palette index 0 into the
        # RGB565 framebuffer.
        self.back_buffer[:] = self.palette_rgb565[0]
        self.z_buffer[:] = CLEAR_DEPTH
        self.vsync_latch = 0

    def set_palette_entry(self, index: int, r: int, g: int, b: int) -> None:
        self.palette[index] = (r, g, b)
        self.palette_rgb565[index] = rgb888_to_rgb565((r, g, b))

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
        edges = np.asarray(quad.edges, dtype=np.int64)
        if quad.uv is not None:
            uv = np.asarray(quad.uv, dtype=np.int64)
            has_uv = 1
        else:
            uv = _EMPTY_UV_NP
            has_uv = 0

        _k_rasterize_quad(
            self.back_buffer,
            self.z_buffer,
            self.width,
            self.height,
            self.palette_rgb565,
            self._textures_np,
            RECIP_LUT_NP,
            1 if self.fog_enabled else 0,
            self.fog_start,
            self.fog_end,
            int(self.palette_rgb565[self.fog_color]),
            self.fog_inv_proj_sq,
            quad.x_min,
            quad.y_min,
            quad.x_max,
            quad.y_max,
            edges,
            quad.z0,
            quad.dz_dx,
            quad.dz_dy,
            quad.tex_or_color,
            quad.flags,
            has_uv,
            uv,
        )
