# Minecraft Source Texture Inputs

This folder contains only source sprites consumed by
`hw/voxel_gpu/scripts/generate_textures.py`.

HUD sprites are kept as their vanilla source frames. The generator decides
whether to preserve the frame (`heart`, `air`) or crop to visible bounds
(`food_full`) before fitting the 16x16 hardware atlas tile.
