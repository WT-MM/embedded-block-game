from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from virtualhw.protocol import QUAD_FLAG_FOG, QUAD_FLAG_TEX, QuadDesc
from virtualhw.raster import (
    TEXTURE_BYTES,
    blend_rgb565,
    rgb888_to_rgb565,
    VirtualGPU,
)


def one_pixel_quad(
    x: int,
    y: int,
    *,
    tex_or_color: int,
    flags: int,
    uv: tuple[int, int, int, int, int, int, int, int, int] | None = None,
) -> QuadDesc:
    return QuadDesc(
        x_min=x,
        y_min=y,
        x_max=x,
        y_max=y,
        edges=((0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0)),
        z0=0,
        dz_dx=0,
        dz_dy=0,
        tex_or_color=tex_or_color,
        flags=flags,
        uv=uv,
    )


def solid_bbox_quad(
    x_min: int,
    y_min: int,
    x_max: int,
    y_max: int,
    *,
    tex_or_color: int,
    flags: int,
) -> QuadDesc:
    return QuadDesc(
        x_min=x_min,
        y_min=y_min,
        x_max=x_max,
        y_max=y_max,
        edges=((0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0)),
        z0=0,
        dz_dx=0,
        dz_dy=0,
        tex_or_color=tex_or_color,
        flags=flags,
        uv=None,
    )


class RasterBehaviorTest(unittest.TestCase):
    def test_texture_coordinates_clamp_to_tile_edge(self) -> None:
        textures = bytearray(TEXTURE_BYTES)
        textures[0] = 1
        textures[15] = 2
        gpu = VirtualGPU(textures=bytes(textures))
        gpu.set_palette_entry(1, 0xFF, 0x00, 0x00)
        gpu.set_palette_entry(2, 0x00, 0x00, 0xFF)

        uv = (
            16 << 16,
            0,
            0,
            0,
            0,
            0,
            1 << 16,
            0,
            0,
        )
        gpu.submit_quads(
            [one_pixel_quad(0, 0, tex_or_color=0, flags=QUAD_FLAG_TEX, uv=uv)]
        )

        self.assertEqual(gpu.back_buffer[0], rgb888_to_rgb565((0x00, 0x00, 0xFF)))

    def test_fog_blends_source_toward_fog_color(self) -> None:
        textures = bytearray(TEXTURE_BYTES)
        textures[0] = 1
        gpu = VirtualGPU(textures=bytes(textures))
        gpu.set_palette_entry(1, 0xFF, 0x00, 0x00)
        gpu.set_palette_entry(2, 0x00, 0x00, 0xFF)
        gpu.set_fog(0, 8 << 8, 2, 1, 0)

        uv = (
            0,
            0,
            0,
            0,
            0,
            0,
            1 << 16,
            0,
            0,
        )
        gpu.submit_quads(
            [
                one_pixel_quad(
                    160,
                    120,
                    tex_or_color=0,
                    flags=QUAD_FLAG_TEX | QUAD_FLAG_FOG,
                    uv=uv,
                )
            ]
        )

        src = rgb888_to_rgb565((0xFF, 0x00, 0x00))
        fog = rgb888_to_rgb565((0x00, 0x00, 0xFF))
        self.assertEqual(
            gpu.back_buffer[120 * 320 + 160],
            blend_rgb565(src, fog, 1),
        )

    def test_descriptor_bbox_is_clamped_like_fpga(self) -> None:
        gpu = VirtualGPU()
        gpu.set_palette_entry(1, 0x12, 0x34, 0x56)
        quad = QuadDesc(
            x_min=-5,
            y_min=0,
            x_max=-1,
            y_max=0,
            edges=((0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0)),
            z0=0,
            dz_dx=0,
            dz_dy=0,
            tex_or_color=1,
            flags=0,
            uv=None,
        )

        gpu.submit_quads([quad])

        self.assertEqual(gpu.back_buffer[0], rgb888_to_rgb565((0x12, 0x34, 0x56)))

    def test_opaque_flat_bbox_fast_path_matches_generic_path(self) -> None:
        fast = VirtualGPU()
        generic = VirtualGPU()
        for gpu in (fast, generic):
            gpu.set_palette_entry(7, 0x44, 0x88, 0xCC)

        fast.submit_quads(
            [solid_bbox_quad(3, 5, 19, 11, tex_or_color=7, flags=0)]
        )
        generic.submit_quads(
            [
                solid_bbox_quad(
                    3,
                    5,
                    19,
                    11,
                    tex_or_color=7,
                    flags=QUAD_FLAG_FOG,
                )
            ]
        )

        self.assertEqual(fast.back_buffer, generic.back_buffer)


if __name__ == "__main__":
    unittest.main()
