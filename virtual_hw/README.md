Virtual Hardware
================

This directory contains a Python virtual GPU that accepts the same palette
updates and packed 64-byte `quad_desc` payloads that the C renderer already
emits for the FPGA path.

What it does
------------
- Listens on a Unix socket, default `/tmp/voxel_gpu.sock`
- Accepts `CLEAR`, `SET_PALETTE`, `SUBMIT_QUADS`, and `FLIP`
- Rasterizes quads into a 320x240 indexed framebuffer plus 16-bit z-buffer
- Opens a scaled desktop monitor window with `pygame`
- Can also run headless and dump frames as `.ppm`

Quick start on Ubuntu
---------------------
Install the optional window dependency:

    python3 -m pip install -r virtual_hw/requirements.txt

Start the server:

    python3 virtual_hw/server.py

In another shell, run the software renderer against it:

    cd sw
    VOXEL_GPU_BACKEND=socket ./tests/renderer_static_test

For the interactive game on Ubuntu:

    cd sw
    make game
    VOXEL_GPU_BACKEND=socket ./game

Useful options
--------------

    python3 virtual_hw/server.py --scale 4
    python3 virtual_hw/server.py --headless --dump-dir /tmp/voxel_frames

Transport modes
---------------
The C renderer now supports:

- `VOXEL_GPU_BACKEND=hw`
  Uses `/dev/voxel_gpu` only. This is the default.
- `VOXEL_GPU_BACKEND=socket`
  Uses the Python virtual GPU only.
- `VOXEL_GPU_BACKEND=tee`
  Sends the same palette/quads/flip commands to both hardware and the socket.

Socket path:

    VOXEL_GPU_SOCKET_PATH=/tmp/voxel_gpu.sock
