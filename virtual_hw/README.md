Virtual Hardware
================

`virtualhw` is a small Python monitor for the voxel GPU protocol. It accepts
the same palette updates and packed 64-byte `quad_desc` stream as the C
renderer, then shows the result in a `pygame` window or writes frames
headlessly as `.ppm`.

Quick Start
-----------

    cd virtual_hw
    uv sync
    uv run virtualhw --scale 4

In another shell:

    cd sw
    make game
    VOXEL_GPU_BACKEND=socket ./game

Useful Commands
---------------

    uv run virtualhw --headless --dump-dir /tmp/voxel_frames
    uv run python -m virtualhw.server

If you activate the venv:

    source .venv/bin/activate
    virtualhw --scale 4

Backends
--------

- `VOXEL_GPU_BACKEND=hw`: real `/dev/voxel_gpu` device. Default.
- `VOXEL_GPU_BACKEND=socket`: Python virtual GPU only.
- `VOXEL_GPU_BACKEND=tee`: send commands to both hardware and the socket.

Optional socket override:

    VOXEL_GPU_SOCKET_PATH=/tmp/voxel_gpu.sock

Compatibility wrappers
----------------------

These still work if you need them:

    python3 virtual_hw/server.py
    python3 virtual_hw/protocol.py
    python3 virtual_hw/raster.py
