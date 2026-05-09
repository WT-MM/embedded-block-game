from __future__ import annotations

from collections.abc import Iterable
import errno
import struct
from dataclasses import dataclass

MAGIC = b"VGPU"
VERSION = 2
DEFAULT_SOCKET_PATH = "/tmp/voxel_gpu.sock"

CMD_CLEAR = 1
CMD_FLIP = 2
CMD_SET_PALETTE = 3
CMD_SUBMIT_QUADS = 4
CMD_GET_STATUS = 5
CMD_GET_FRAME_COUNT = 6
CMD_SET_FOG = 7

QUAD_FLAG_TEX = 1 << 0
QUAD_FLAG_ZTEST = 1 << 1
QUAD_FLAG_ALPHA_KEY = 1 << 2
QUAD_FLAG_FOG = 1 << 3
QUAD_LIGHT_SHIFT = 4
QUAD_LIGHT_MASK = 3 << QUAD_LIGHT_SHIFT
QUAD_ALPHA_SHIFT = 6
QUAD_ALPHA_MASK = 3 << QUAD_ALPHA_SHIFT
TEX_REPEAT_UV = 1 << 7

HEADER = struct.Struct("<4sHHI")
REPLY = struct.Struct("<4sHhI")
PALETTE_ENTRY = struct.Struct("<BBBB")
FOG_STATE = struct.Struct("<HHBBH")
STATUS_REPLY = struct.Struct("<IIBBBB")
FRAME_COUNT_REPLY = struct.Struct("<I")
QUAD_DESC = struct.Struct("<hhhh" + ("iii" * 4) + "HhhBB")
QUAD_DESC_UV = struct.Struct("<iiiiiiiii")

Edge = tuple[int, int, int]
QuadEdges = tuple[Edge, Edge, Edge, Edge]


class ProtocolError(RuntimeError):
    pass


def _is_disconnect_error(exc: OSError) -> bool:
    return exc.errno in {
        errno.EPIPE,
        errno.ECONNRESET,
        errno.ENOTCONN,
        errno.EBADF,
        errno.ECONNABORTED,
        errno.ESHUTDOWN,
    }


@dataclass(frozen=True)
class QuadDesc:
    x_min: int
    y_min: int
    x_max: int
    y_max: int
    edges: QuadEdges
    z0: int
    dz_dx: int
    dz_dy: int
    tex_or_color: int
    flags: int
    uv: tuple[int, int, int, int, int, int, int, int, int] | None = None


def recv_exact(sock, size: int) -> bytes:
    data = bytearray(size)
    view = memoryview(data)
    while view:
        try:
            count = sock.recv_into(view)
        except OSError as exc:
            if _is_disconnect_error(exc):
                raise EOFError from exc
            raise
        if count == 0:
            raise EOFError
        view = view[count:]
    return bytes(data)


def recv_request(sock) -> tuple[int, bytes]:
    raw = recv_exact(sock, HEADER.size)
    magic, version, opcode, payload_size = HEADER.unpack(raw)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {magic!r}")
    if version != VERSION:
        raise ProtocolError(f"unsupported protocol version {version}")
    payload = recv_exact(sock, payload_size) if payload_size else b""
    return opcode, payload


def send_reply(sock, status: int, payload: bytes = b"") -> None:
    header = REPLY.pack(MAGIC, VERSION, status, len(payload))
    try:
        sock.sendall(header)
        if payload:
            sock.sendall(payload)
    except OSError as exc:
        if _is_disconnect_error(exc):
            raise EOFError from exc
        raise


def parse_palette_entry(payload: bytes) -> tuple[int, int, int, int]:
    if len(payload) != PALETTE_ENTRY.size:
        raise ProtocolError(f"palette payload must be {PALETTE_ENTRY.size} bytes")
    return PALETTE_ENTRY.unpack(payload)


def parse_fog_state(payload: bytes) -> tuple[int, int, int, int, int]:
    if len(payload) != FOG_STATE.size:
        raise ProtocolError(f"fog payload must be {FOG_STATE.size} bytes")
    return FOG_STATE.unpack(payload)


def iter_quads(payload: bytes) -> Iterable[QuadDesc]:
    offset = 0

    while offset < len(payload):
        if offset + QUAD_DESC.size > len(payload):
            raise ProtocolError("truncated base quad descriptor")

        fields = QUAD_DESC.unpack_from(payload, offset)
        offset += QUAD_DESC.size
        edges: QuadEdges = (
            (fields[4], fields[5], fields[6]),
            (fields[7], fields[8], fields[9]),
            (fields[10], fields[11], fields[12]),
            (fields[13], fields[14], fields[15]),
        )

        flags = fields[20]
        uv = None
        if flags & QUAD_FLAG_TEX:
            if offset + QUAD_DESC_UV.size > len(payload):
                raise ProtocolError("truncated textured quad UV block")
            uv = QUAD_DESC_UV.unpack_from(payload, offset)
            offset += QUAD_DESC_UV.size

        yield QuadDesc(
            x_min=fields[0],
            y_min=fields[1],
            x_max=fields[2],
            y_max=fields[3],
            edges=edges,
            z0=fields[16],
            dz_dx=fields[17],
            dz_dy=fields[18],
            tex_or_color=fields[19],
            flags=flags,
            uv=uv,
        )


def pack_status(
    raw: int, fifo_count: int, busy: int, fifo_full: int, fifo_empty: int, vsync: int
) -> bytes:
    return STATUS_REPLY.pack(raw, fifo_count, busy, fifo_full, fifo_empty, vsync)


def pack_frame_count(frame_count: int) -> bytes:
    return FRAME_COUNT_REPLY.pack(frame_count)
