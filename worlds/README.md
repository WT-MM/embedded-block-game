World Saves
===========

The game now persists modified chunks under this directory by default when you
launch it from `sw/`.

Default layout:

    worlds/
      default/
        world.meta
        chunks/
          <chunk_x>_<chunk_z>.chk

Notes:

- `world.meta` stores the world seed and chunk-format parameters.
- `chunks/*.chk` stores full 16x16x16 chunk snapshots for modified chunks only.
- Procedural terrain is still generated from the seed first; saved chunk files
  are then overlaid on top.
- Set `VOXEL_WORLD_DIR=/path/to/world` to use a different save root.
