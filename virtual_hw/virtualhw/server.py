from __future__ import annotations

import argparse
import errno
import os
import select
import socket
import sys
import time
from pathlib import Path

from .protocol import (
    CMD_CLEAR,
    CMD_FLIP,
    CMD_GET_FRAME_COUNT,
    CMD_GET_STATUS,
    CMD_SET_FOG,
    CMD_SET_PALETTE,
    CMD_SUBMIT_QUADS,
    DEFAULT_SOCKET_PATH,
    ProtocolError,
    iter_quads,
    pack_frame_count,
    pack_status,
    parse_fog_state,
    parse_palette_entry,
    recv_request,
    send_reply,
)
from .raster import VirtualGPU, load_texture_mif, rgb565_to_rgb888

SCREEN_WIDTH = 320
SCREEN_HEIGHT = 240
POLL_TIMEOUT = 1.0 / 60.0
DEFAULT_TEXTURE_PATH = Path(__file__).resolve().parents[2] / "hw" / "textures.mif"
DEFAULT_PERF_LOG_INTERVAL = 1.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Virtual voxel GPU monitor")
    parser.add_argument(
        "--socket-path",
        default=DEFAULT_SOCKET_PATH,
        help=f"Unix socket path (default: {DEFAULT_SOCKET_PATH})",
    )
    parser.add_argument(
        "--scale", type=int, default=3, help="window scale multiplier (default: 3)"
    )
    parser.add_argument(
        "--headless", action="store_true", help="run without opening a window"
    )
    parser.add_argument(
        "--dump-dir", help="optional directory for writing flipped frames as PPMs"
    )
    parser.add_argument(
        "--textures",
        default=str(DEFAULT_TEXTURE_PATH),
        help=f"texture atlas MIF path (default: {DEFAULT_TEXTURE_PATH})",
    )
    parser.add_argument(
        "--perf-log-interval",
        type=float,
        default=DEFAULT_PERF_LOG_INTERVAL,
        help=(
            "seconds between virtual GPU performance logs; "
            "use 0 to disable (default: 1.0)"
        ),
    )
    return parser.parse_args()


class PerfLogger:
    def __init__(self, interval_seconds: float) -> None:
        self.interval_seconds = interval_seconds
        self.reset()

    def reset(self) -> None:
        self.window_start = time.perf_counter()
        self.frames = 0
        self.submits = 0
        self.quads = 0
        self.clear_seconds = 0.0
        self.submit_seconds = 0.0
        self.flip_seconds = 0.0
        self.present_seconds = 0.0
        self.dump_seconds = 0.0

    @property
    def enabled(self) -> bool:
        return self.interval_seconds > 0.0

    def record_clear(self, seconds: float) -> None:
        self.clear_seconds += seconds

    def record_submit(self, seconds: float, quads: int) -> None:
        self.submits += 1
        self.quads += quads
        self.submit_seconds += seconds

    def record_flip(
        self, flip_seconds: float, present_seconds: float, dump_seconds: float
    ) -> None:
        self.frames += 1
        self.flip_seconds += flip_seconds
        self.present_seconds += present_seconds
        self.dump_seconds += dump_seconds

    def maybe_log(self) -> None:
        if not self.enabled:
            return

        now = time.perf_counter()
        elapsed = now - self.window_start
        if elapsed < self.interval_seconds:
            return

        frame_div = self.frames if self.frames > 0 else 1
        submit_div = self.submits if self.submits > 0 else 1
        fps = self.frames / elapsed if elapsed > 0.0 else 0.0
        frame_ms = (elapsed / self.frames * 1000.0) if self.frames > 0 else 0.0

        print(
            "virtualhw perf: "
            f"fps={fps:5.1f} frame={frame_ms:6.2f}ms "
            f"quads/frame={self.quads / frame_div:6.1f} "
            f"submit={self.submit_seconds / submit_div * 1000.0:6.2f}ms "
            f"clear={self.clear_seconds / frame_div * 1000.0:5.2f}ms "
            f"flip={self.flip_seconds / frame_div * 1000.0:5.2f}ms "
            f"present={self.present_seconds / frame_div * 1000.0:5.2f}ms "
            f"dump={self.dump_seconds / frame_div * 1000.0:5.2f}ms",
            flush=True,
        )

        self.reset()


class Monitor:
    def __init__(self, width: int, height: int, scale: int, headless: bool) -> None:
        self.width = width
        self.height = height
        self.scale = scale
        self.headless = headless
        self._window = None
        self._pygame = None
        self._rgb_buffer = bytearray(width * height * 3)

        if headless:
            return

        try:
            import pygame  # type: ignore
        except ImportError:
            print(
                "virtualhw: pygame is not installed. Run `uv sync` in virtual_hw "
                "or install the package into your environment first.",
                file=sys.stderr,
            )
            raise

        pygame.init()
        self._pygame = pygame
        self._window = pygame.display.set_mode((width * scale, height * scale))
        pygame.display.set_caption("Virtual Voxel GPU")

    def close(self) -> None:
        if self._pygame is not None:
            self._pygame.quit()

    def pump(self) -> None:
        if self.headless:
            return

        assert self._pygame is not None

        for event in self._pygame.event.get():
            if event.type == self._pygame.QUIT:
                raise KeyboardInterrupt

    def _frame_to_rgb(self, frame) -> bytearray:
        out = self._rgb_buffer

        for i, rgb565 in enumerate(frame):
            r, g, b = rgb565_to_rgb888(rgb565)
            base = i * 3
            out[base] = r
            out[base + 1] = g
            out[base + 2] = b

        return self._rgb_buffer

    def present(self, gpu: VirtualGPU) -> None:
        if self.headless:
            return

        self.pump()

        assert self._window is not None

        rgb = self._frame_to_rgb(gpu.front_buffer)
        surface = self._pygame.image.frombuffer(rgb, (self.width, self.height), "RGB")
        if self.scale != 1:
            surface = self._pygame.transform.scale(
                surface, (self.width * self.scale, self.height * self.scale)
            )

        self._window.blit(surface, (0, 0))
        self._pygame.display.flip()

    def rgb_frame(self, gpu: VirtualGPU) -> bytearray:
        return self._frame_to_rgb(gpu.front_buffer)


def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    path.write_bytes(header + rgb)


def handle_request(
    gpu: VirtualGPU,
    monitor: Monitor,
    dump_dir: Path | None,
    perf: PerfLogger,
    opcode: int,
    payload: bytes,
) -> bytes:
    if opcode == CMD_CLEAR:
        start = time.perf_counter()
        gpu.clear()
        perf.record_clear(time.perf_counter() - start)
        return b""
    if opcode == CMD_FLIP:
        flip_start = time.perf_counter()
        gpu.flip()
        present_start = time.perf_counter()
        monitor.present(gpu)
        dump_start = time.perf_counter()
        if dump_dir is not None:
            dump_path = dump_dir / f"frame_{gpu.frame_count:06d}.ppm"
            write_ppm(dump_path, gpu.width, gpu.height, monitor.rgb_frame(gpu))
        perf.record_flip(
            present_start - flip_start,
            dump_start - present_start,
            time.perf_counter() - dump_start,
        )
        return b""
    if opcode == CMD_SET_PALETTE:
        gpu.set_palette_entry(*parse_palette_entry(payload))
        return b""
    if opcode == CMD_SET_FOG:
        gpu.set_fog(*parse_fog_state(payload))
        return b""
    if opcode == CMD_SUBMIT_QUADS:
        start = time.perf_counter()
        quad_count = gpu.submit_quads(iter_quads(payload))
        perf.record_submit(time.perf_counter() - start, quad_count)
        return b""
    if opcode == CMD_GET_STATUS:
        return pack_status(*gpu.status_tuple())
    if opcode == CMD_GET_FRAME_COUNT:
        return pack_frame_count(gpu.frame_count)
    raise ProtocolError(f"unsupported opcode {opcode}")


def serve_client(
    conn: socket.socket,
    gpu: VirtualGPU,
    monitor: Monitor,
    dump_dir: Path | None,
    perf: PerfLogger,
) -> None:
    while True:
        monitor.pump()
        readable, _, _ = select.select([conn], [], [], POLL_TIMEOUT)
        if not readable:
            continue

        try:
            opcode, payload = recv_request(conn)
        except EOFError:
            return
        except ProtocolError as exc:
            print(f"virtualhw: protocol error: {exc}", file=sys.stderr)
            try:
                send_reply(conn, -errno.EPROTO)
            except EOFError:
                return
            return

        try:
            reply_payload = handle_request(
                gpu, monitor, dump_dir, perf, opcode, payload
            )
            send_reply(conn, 0, reply_payload)
            perf.maybe_log()
        except EOFError:
            return
        except ProtocolError as exc:
            print(f"virtualhw: request failed: {exc}", file=sys.stderr)
            try:
                send_reply(conn, -errno.EINVAL)
            except EOFError:
                return
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # pragma: no cover - defensive debug path
            print(f"virtualhw: unexpected error: {exc}", file=sys.stderr)
            try:
                send_reply(conn, -errno.EIO)
            except EOFError:
                return


def main() -> int:
    args = parse_args()
    socket_path = Path(args.socket_path)
    dump_dir = Path(args.dump_dir) if args.dump_dir else None
    texture_path = Path(args.textures)

    if dump_dir is not None:
        dump_dir.mkdir(parents=True, exist_ok=True)

    socket_path.parent.mkdir(parents=True, exist_ok=True)
    socket_path.unlink(missing_ok=True)

    monitor = Monitor(SCREEN_WIDTH, SCREEN_HEIGHT, args.scale, args.headless)
    try:
        textures = load_texture_mif(texture_path)
    except (OSError, ValueError) as exc:
        print(f"virtualhw: failed to load textures: {exc}", file=sys.stderr)
        monitor.close()
        return 1

    gpu = VirtualGPU(SCREEN_WIDTH, SCREEN_HEIGHT, textures=textures)
    perf = PerfLogger(args.perf_log_interval)

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(socket_path))
        os.chmod(socket_path, 0o666)
        server.listen(1)

        print(f"virtualhw: listening on {socket_path}")
        print(f"virtualhw: textures={texture_path}")
        if perf.enabled:
            print(f"virtualhw: perf logs every {args.perf_log_interval:.1f}s")
        print("virtualhw: headless mode" if args.headless else "virtualhw: pygame window ready")

        try:
            while True:
                monitor.pump()
                readable, _, _ = select.select([server], [], [], POLL_TIMEOUT)
                if not readable:
                    continue

                conn, _ = server.accept()
                print("virtualhw: client connected")
                perf.reset()
                with conn:
                    serve_client(conn, gpu, monitor, dump_dir, perf)
                print("virtualhw: client disconnected")
        except KeyboardInterrupt:
            print("\nvirtualhw: shutting down")
        finally:
            monitor.close()
            socket_path.unlink(missing_ok=True)

    return 0
