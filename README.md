Embedded Block Game
===================

Quick Start: Virtual Voxel GPU
------------------------------

Run the Python virtual GPU in one shell:

    cd virtual_hw
    uv sync
    uv run virtualhw --scale 4

Run the game against it in another shell:

    cd sw
    make game
    VOXEL_GPU_BACKEND=socket ./game

Useful Variants
---------------

Mirror commands to both the FPGA driver and the virtual GPU:

    cd sw
    VOXEL_GPU_BACKEND=tee ./game

Run the virtual GPU headless and dump frames:

    cd virtual_hw
    uv run virtualhw --headless --dump-dir /tmp/voxel_frames

Notes
-----

- `VOXEL_GPU_BACKEND=hw` is the default and uses `/dev/voxel_gpu`.
- `VOXEL_GPU_BACKEND=socket` avoids the kernel driver entirely.
- `./game` reads keyboard and mouse input from `/dev/input/event*` on Linux.

If input only works under `sudo`, add your user to the `input` group and start
a new login session:

    sudo usermod -aG input $USER

More Detail
-----------

- `sw/README` covers the kernel module, renderer tests, and transport modes.
- `virtual_hw/README.md` covers the Python virtual GPU package and options.
