# Minecraft Source Texture Inputs

This folder contains only source sprites consumed by
`hw/voxel_gpu/scripts/generate_textures.py`.

HUD sprites are kept as their vanilla source frames. The generator decides
whether to preserve the frame (`heart`, `air`) or crop to visible bounds
(`food_full`) before fitting the 16x16 hardware atlas tile.

Additional vanilla block/placeable inputs were pulled from Mojang's official
Bedrock sample resource pack:

https://github.com/Mojang/bedrock-samples/releases/tag/v1.26.20.4

Source path inside that release:

`resource_pack/textures/blocks/`

These files remain Mojang assets and are subject to the Minecraft EULA.
