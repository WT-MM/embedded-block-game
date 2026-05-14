Virtual Hardware
================

`virtualhw` is a small Python monitor for the voxel GPU protocol. It accepts
the same palette updates and packed 64-byte `quad_desc` stream as the C
renderer, then shows the result in a `pygame` window or writes frames
headlessly as `.ppm`.

Run With The Game
-----------------

    cd virtual_hw
    uv sync
    uv run virtualhw --scale 4

For the JIT-compiled rasterizer (roughly two orders of magnitude faster on
the per-pixel path), install the optional `jit` extra once:

    uv sync --extra jit
    uv run virtualhw --scale 4

The first frame pays a one-time Numba compile cost (~1s); subsequent runs
reuse the on-disk cache. Set `VIRTUALHW_JIT=0` to force the pure-Python
reference path — useful when debugging bit-exact divergence against the
FPGA. You can probe which path is active with:

    uv run python -c "from virtualhw.raster import JIT_ENABLED; print(JIT_ENABLED)"

In another shell:

    cd sw
    make game
    VOXEL_GPU_BACKEND=socket ./game

Useful Commands
---------------

    uv run virtualhw --headless --dump-dir /tmp/voxel_frames
    uv run python -m virtualhw.server
    cd ../sw && VOXEL_GPU_BACKEND=socket ./tests/renderer_static_test

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

Linux input note:

`./game` reads `/dev/input/event*`. If keyboard or mouse input only works under
`sudo`, add your user to the `input` group and start a new login session.
Relative mice and absolute VM tablet devices are both accepted for look input.
Use `VOXEL_MOUSE_INVERT_X=1` and/or `VOXEL_MOUSE_INVERT_Y=1` to flip axes.
Use `VOXEL_MOUSE_SENS=0.004` to override mouse sensitivity at launch.
