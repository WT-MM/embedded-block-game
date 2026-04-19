from __future__ import annotations

import argparse
import errno
import os
import socket
import sys
from pathlib import Path
from typing import Optional

from protocol import (
    CMD_CLEAR,
    CMD_FLIP,
    CMD_GET_FRAME_COUNT,
    CMD_GET_STATUS,
    CMD_SET_PALETTE,
    CMD_SUBMIT_QUADS,
    DEFAULT_SOCKET_PATH,
    ProtocolError,
    iter_quads,
    pack_frame_count,
    pack_status,
    parse_palette_entry,
    recv_request,
    send_reply,
)
from raster import VirtualGPU


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Virtual voxel GPU monitor")
    parser.add_argument("--socket-path", default=DEFAULT_SOCKET_PATH,
                        help=f"Unix socket path (default: {DEFAULT_SOCKET_PATH})")
    parser.add_argument("--scale", type=int, default=3,
                        help="window scale multiplier (default: 3)")
    parser.add_argument("--headless", action="store_true",
                        help="run without opening a window")
    parser.add_argument("--dump-dir",
                        help="optional directory for writing flipped frames as PPMs")
    return parser.parse_args()


class Monitor:
    def __init__(self, width: int, height: int, scale: int, headless: bool) -> None:
        self.width = width
        self.height = height
        self.scale = scale
        self.headless = headless
        self._window = None
        self._pygame = None
        self._rgb_buffer = bytearray(width * height * 3)
        self._red_lut = bytes(256)
        self._green_lut = bytes(256)
        self._blue_lut = bytes(256)

        if headless:
            return

        try:
            import pygame  # type: ignore
        except ImportError:
            print("virtual_hw: pygame is not installed; use --headless or install it with",
                  "`python3 -m pip install -r virtual_hw/requirements.txt`.",
                  file=sys.stderr)
            raise

        pygame.init()
        self._pygame = pygame
        self._window = pygame.display.set_mode((width * scale, height * scale))
        pygame.display.set_caption("Virtual Voxel GPU")

    def _refresh_palette_luts(self, palette) -> None:
        self._red_lut = bytes(rgb[0] for rgb in palette)
        self._green_lut = bytes(rgb[1] for rgb in palette)
        self._blue_lut = bytes(rgb[2] for rgb in palette)

    def _frame_to_rgb(self, frame: bytearray) -> bytearray:
        frame_bytes = bytes(frame)
        red = frame_bytes.translate(self._red_lut)
        green = frame_bytes.translate(self._green_lut)
        blue = frame_bytes.translate(self._blue_lut)
        self._rgb_buffer[0::3] = red
        self._rgb_buffer[1::3] = green
        self._rgb_buffer[2::3] = blue
        return self._rgb_buffer

    def present(self, gpu: VirtualGPU) -> None:
        if gpu.palette_dirty:
            self._refresh_palette_luts(gpu.palette)
            gpu.palette_dirty = False

        if self.headless:
            return

        assert self._pygame is not None
        assert self._window is not None

        for event in self._pygame.event.get():
            if event.type == self._pygame.QUIT:
                raise KeyboardInterrupt

        rgb = self._frame_to_rgb(gpu.front_buffer)
        surface = self._pygame.image.frombuffer(rgb, (self.width, self.height), "RGB")
        if self.scale != 1:
            surface = self._pygame.transform.scale(
                surface, (self.width * self.scale, self.height * self.scale)
            )

        self._window.blit(surface, (0, 0))
        self._pygame.display.flip()

    def rgb_frame(self, gpu: VirtualGPU) -> bytearray:
        if gpu.palette_dirty:
            self._refresh_palette_luts(gpu.palette)
            gpu.palette_dirty = False
        return self._frame_to_rgb(gpu.front_buffer)


def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    path.write_bytes(header + rgb)


def handle_request(gpu: VirtualGPU, monitor: Monitor,
                   dump_dir: Optional[Path], opcode: int, payload: bytes) -> bytes:
    if opcode == CMD_CLEAR:
        gpu.clear()
        return b""

    if opcode == CMD_FLIP:
        gpu.flip()
        monitor.present(gpu)
        if dump_dir is not None:
            dump_path = dump_dir / f"frame_{gpu.frame_count:06d}.ppm"
            write_ppm(dump_path, gpu.width, gpu.height, monitor.rgb_frame(gpu))
        return b""

    if opcode == CMD_SET_PALETTE:
        index, r, g, b = parse_palette_entry(payload)
        gpu.set_palette_entry(index, r, g, b)
        return b""

    if opcode == CMD_SUBMIT_QUADS:
        gpu.submit_quads(iter_quads(payload))
        return b""

    if opcode == CMD_GET_STATUS:
        return pack_status(*gpu.status_tuple())

    if opcode == CMD_GET_FRAME_COUNT:
        return pack_frame_count(gpu.frame_count)

    raise ProtocolError(f"unsupported opcode {opcode}")


def serve_client(conn: socket.socket, gpu: VirtualGPU, monitor: Monitor,
                 dump_dir: Optional[Path]) -> None:
    while True:
        try:
            opcode, payload = recv_request(conn)
        except EOFError:
            return
        except ProtocolError as exc:
            print(f"virtual_hw: protocol error: {exc}", file=sys.stderr)
            send_reply(conn, -errno.EPROTO)
            return

        try:
            reply_payload = handle_request(gpu, monitor, dump_dir, opcode, payload)
            send_reply(conn, 0, reply_payload)
        except ProtocolError as exc:
            print(f"virtual_hw: request failed: {exc}", file=sys.stderr)
            send_reply(conn, -errno.EINVAL)
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # pragma: no cover - defensive debug path
            print(f"virtual_hw: unexpected error: {exc}", file=sys.stderr)
            send_reply(conn, -errno.EIO)


def main() -> int:
    args = parse_args()
    socket_path = Path(args.socket_path)
    dump_dir = Path(args.dump_dir) if args.dump_dir else None

    if dump_dir is not None:
        dump_dir.mkdir(parents=True, exist_ok=True)

    if socket_path.exists():
        socket_path.unlink()
    socket_path.parent.mkdir(parents=True, exist_ok=True)

    monitor = Monitor(320, 240, args.scale, args.headless)
    gpu = VirtualGPU(320, 240)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o666)
    server.listen(1)

    print(f"virtual_hw: listening on {socket_path}")
    if not args.headless:
        print("virtual_hw: pygame window ready")
    else:
        print("virtual_hw: headless mode")

    try:
        while True:
            conn, _ = server.accept()
            print("virtual_hw: client connected")
            with conn:
                serve_client(conn, gpu, monitor, dump_dir)
            print("virtual_hw: client disconnected")
    except KeyboardInterrupt:
        print("\nvirtual_hw: shutting down")
    finally:
        server.close()
        if socket_path.exists():
            socket_path.unlink()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
