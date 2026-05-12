#!/usr/bin/env python3
"""Generate source-grounded HPS game-logic documentation and diagrams."""

from __future__ import annotations

from pathlib import Path
import re

from diagram_source_model import DOCS, DIAGRAMS, ROOT, class_defs, ensure_dirs, mermaid_header, rel, write


GAME_C = ROOT / "sw" / "game.c"
GAME_HOME_C = ROOT / "sw" / "game_home.c"
WORLD_H = ROOT / "sw" / "world.h"
WORLD_C = ROOT / "sw" / "world.c"
WORLD_GEN_C = ROOT / "sw" / "world_gen.c"
PLAYER_H = ROOT / "sw" / "player_physics.h"
PLAYER_C = ROOT / "sw" / "player_physics.c"
INPUT_H = ROOT / "sw" / "input.h"
CHAT_H = ROOT / "sw" / "chat.h"
PAUSE_H = ROOT / "sw" / "pause_menu.h"
INVENTORY_H = ROOT / "sw" / "inventory.h"
GAME_ITEMS_H = ROOT / "sw" / "game_items.h"
BLOCK_TYPES_H = ROOT / "sw" / "block_types.h"
COMMAND_H = ROOT / "sw" / "command_parser.h"
MESH_WORKER_H = ROOT / "sw" / "mesh_worker.h"
GEN_WORKER_H = ROOT / "sw" / "gen_worker.h"
RENDERER_H = ROOT / "sw" / "renderer.h"
RENDERER_C = ROOT / "sw" / "renderer.c"
GPU_TRANSPORT_C = ROOT / "sw" / "gpu_transport.c"
GPU_TRANSPORT_H = ROOT / "sw" / "gpu_transport.h"
VOXEL_GPU_H = ROOT / "sw" / "voxel_gpu.h"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def line_for(path: Path, pattern: str) -> int | None:
    regex = re.compile(pattern)
    for index, line in enumerate(read(path).splitlines(), start=1):
        if regex.search(line):
            return index
    return None


def source_ref(path: Path, pattern: str) -> str:
    line = line_for(path, pattern)
    if line is None:
        return f"`{rel(path)}:?`"
    return f"`{rel(path)}:{line}`"


def function_ref(path: Path, name: str) -> str:
    regex = re.compile(
        rf"^\s*(?:static\s+)?(?:inline\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*\b{name}\s*\("
    )
    lines = read(path).splitlines()
    for index, line in enumerate(lines, start=1):
        if not regex.search(line):
            continue
        snippet = "\n".join(lines[index - 1 : index + 8])
        brace = snippet.find("{")
        semi = snippet.find(";")
        if brace != -1 and (semi == -1 or brace < semi):
            return f"`{rel(path)}:{index}`"
    return source_ref(path, rf"\b{name}\s*\(")


def macro_ref(path: Path, name: str) -> str:
    return source_ref(path, rf"^\s*#define\s+{re.escape(name)}\b")


def struct_ref(path: Path, name: str) -> str:
    return source_ref(path, rf"^\s*(?:typedef\s+)?struct(?:\s+{re.escape(name)})?\b")


def function_inventory() -> str:
    rows = [
        ("Frame owner", "main", GAME_C, "Owns process startup, home menu handoff, frame loop, render submission, shutdown."),
        ("World select", "run_home_menu", GAME_HOME_C, "Selects/creates the saved world and returns seed/path/options."),
        ("World init", "world_init_infinite_procedural", WORLD_C, "Allocates streaming window, persistence metadata, seed/options, and initial chunks."),
        ("Streaming", "world_stream_around", WORLD_C, "Recenters loaded chunk window around player position."),
        ("Environment", "world_water_tick", WORLD_C, "Runs fluid and gravity-block environment tick."),
        ("Redstone", "world_update_redstone", WORLD_C, "Runs pulses, repeaters, comparators/wires/torches/plates."),
        ("Falling blocks", "world_update_falling_blocks", WORLD_C, "Advances smooth falling sand/gravel entities."),
        ("Physics", "player_update", PLAYER_C, "Applies movement, gravity/fly modes, water drag/flow, collision, eye height."),
        ("Block targeting", "trace_target_block", GAME_C, "Ray-steps from camera and resolves hit/place target."),
        ("Block break", "break_block_target", GAME_C, "Mutates world on block break and handles special block side effects."),
        ("Block place", "try_place_targeted_block", GAME_C, "Validates support/replacement and calls world block mutation."),
        ("Buckets", "try_use_held_bucket", GAME_C, "Dispatches empty/fill bucket behavior to fluid source placement/removal."),
        ("Doors", "try_toggle_targeted_door", GAME_C, "Toggles door state based on targeted door half."),
        ("Redstone input", "try_press_targeted_button", GAME_C, "Starts button pulse through world_press_button."),
        ("Lever input", "try_toggle_targeted_lever", GAME_C, "Toggles lever state through world_toggle_lever."),
        ("Repeater input", "try_cycle_targeted_repeater", GAME_C, "Changes repeater delay metadata."),
        ("Furnace sim", "furnace_states_update", GAME_C, "Advances active furnace timers and output/fuel transitions."),
        ("Commands", "execute_chat_command", GAME_C, "Parses and dispatches slash commands."),
        ("Render begin", "renderer_begin_frame", RENDERER_C, "Clears per-frame quad state and starts bin-during-emit."),
        ("World render", "renderer_draw_world", RENDERER_C, "Reads live ChunkMesh snapshots and emits visible faces."),
        ("Descriptor build", "stage_prepared_quad", RENDERER_C, "Builds quad_desc/quad_desc_uv packed FPGA descriptors."),
        ("Render end", "renderer_end_frame", RENDERER_C, "Clears/flips/submits descriptors to the GPU transport."),
        ("GPU submit", "gpu_transport_submit_descriptors", GPU_TRANSPORT_C, "Submits descriptors to HW and optional virtual socket backend."),
    ]
    lines = [
        "| Area | Function | Source | Role |",
        "|---|---|---|---|",
    ]
    for area, name, path, role in rows:
        lines.append(f"| {area} | `{name}()` | {function_ref(path, name)} | {role} |")
    return "\n".join(lines)


def source_inventory() -> str:
    rows = [
        ("Game loop/state", "sw/game.c", "`main()`, `BlockTarget`, `FurnaceState`, UI helpers, command dispatch, targeting, interactions"),
        ("Home menu/world selection", "sw/game_home.c/.h", "`SelectedWorld`, `run_home_menu()`"),
        ("Input", "sw/input.c/.h", "`InputState`, edge-consume helpers, text queue, pointer capture"),
        ("Player physics", "sw/player_physics.c/.h", "`Player`, `PlayerMode`, gravity/fly/water/collision update"),
        ("World/chunks", "sw/world.c/.h", "`VoxelWorld`, `Chunk`, `ChunkMesh`, streaming, lighting, redstone, fluids, persistence"),
        ("Terrain generation", "sw/world_gen.c/.h", "Biome noise, chunk terrain generation, trees/ores/lava/desert/ocean placement"),
        ("Async workers", "sw/gen_worker.c/.h, sw/mesh_worker.c/.h", "Background chunk generation and mesh rebuild queues"),
        ("Block registry", "sw/block_types.c/.h", "`BlockID`, render model, texture IDs, transparency/passability/redstone helpers"),
        ("Inventory/items", "sw/inventory.c/.h, sw/game_items.c/.h", "`SurvivalInventory`, recipes, item drops/entities, food/tools/fuel"),
        ("Chat/commands/UI", "sw/chat.c/.h, sw/command_parser.c/.h, sw/pause_menu.c/.h", "Text input, command AST, pause settings"),
        ("Renderer handoff", "sw/renderer.c/.h", "`RenderContext`, `Camera`, `RenderQuad`, world/UI draw and descriptor generation"),
        ("GPU transport/interface", "sw/gpu_transport.c/.h, sw/voxel_gpu.h", "Descriptor binning/submission, ioctl ABI, register/descriptor layout"),
    ]
    lines = [
        "| Subsystem | Source files | Main names extracted from source |",
        "|---|---|---|",
    ]
    for subsystem, files, names in rows:
        lines.append(f"| {subsystem} | `{files}` | {names} |")
    return "\n".join(lines)


def breakdown_md() -> str:
    refs = {
        "defaults": source_ref(GAME_C, r"^#define DEFAULT_MOUSE_SENS"),
        "physics_hz": macro_ref(GAME_C, "PHYSICS_HZ"),
        "max_frame_dt": macro_ref(GAME_C, "MAX_FRAME_DT"),
        "target_fps": macro_ref(GAME_C, "DEFAULT_TARGET_FPS"),
        "reach": macro_ref(GAME_C, "BLOCK_REACH_DISTANCE"),
        "env_tick": source_ref(GAME_C, r"^\s*#define ENVIRONMENT_TICK_INTERVAL"),
        "redstone_tick": source_ref(GAME_C, r"^\s*#define REDSTONE_TICK_INTERVAL"),
        "main": function_ref(GAME_C, "main"),
        "startup": source_ref(GAME_C, r"renderer_init\(\)"),
        "home": function_ref(GAME_HOME_C, "run_home_menu"),
        "world_init": function_ref(WORLD_C, "world_init_infinite_procedural"),
        "worker_start": source_ref(GAME_C, r"mesh_worker_start\(&world\)"),
        "loop": source_ref(GAME_C, r"^\s*while \(!inp\.quit\)"),
        "env_loop": source_ref(GAME_C, r"world_water_tick\(&world\)"),
        "redstone_loop": source_ref(GAME_C, r"world_update_redstone\(&world"),
        "falling_loop": source_ref(GAME_C, r"world_update_falling_blocks\(&world"),
        "input": source_ref(GAME_C, r"input_update\(&inp\);"),
        "chat_enter": source_ref(GAME_C, r"execute_chat_command\(&chat"),
        "pointer": source_ref(GAME_C, r"input_set_pointer_capture\(&inp"),
        "look": source_ref(GAME_C, r"cam\.yaw\s+\+="),
        "wish": source_ref(GAME_C, r"float wish_x = 0\.0f"),
        "physics": source_ref(GAME_C, r"player_update\(&player"),
        "stream": source_ref(GAME_C, r"world_stream_around\(&world"),
        "hazards": source_ref(GAME_C, r"camera_underwater = camera_is_underwater"),
        "items": source_ref(GAME_C, r"item_entities_update\(&item_drops"),
        "interactions": source_ref(GAME_C, r"else if \(!paused && !chat_is_open\(&chat\)"),
        "workers": source_ref(GAME_C, r"gen_worker_drain_pending\(&world\)"),
        "render": source_ref(GAME_C, r"renderer_begin_frame\(ctx\)"),
        "render_end": source_ref(GAME_C, r"renderer_end_frame\(ctx\)"),
        "sleep": source_ref(GAME_C, r"nanosleep\(&ts"),
        "shutdown": source_ref(GAME_C, r"gen_worker_stop\(\)"),
        "input_state": source_ref(INPUT_H, r"\}\s*InputState;"),
        "player_struct": source_ref(PLAYER_H, r"\}\s*Player;"),
        "world_struct": source_ref(WORLD_H, r"typedef struct VoxelWorld"),
        "chunk_struct": source_ref(WORLD_H, r"\}\s*Chunk;"),
        "chunkmesh": source_ref(WORLD_H, r"typedef struct ChunkMesh"),
        "inventory": source_ref(INVENTORY_H, r"\}\s*SurvivalInventory;"),
        "items_pool": source_ref(GAME_ITEMS_H, r"\}\s*ItemEntityPool;"),
        "blocks": source_ref(BLOCK_TYPES_H, r"BLOCK_AIR\s*="),
        "commands": source_ref(COMMAND_H, r"GAME_COMMAND_KIND_NONE"),
        "renderer_begin": function_ref(RENDERER_C, "renderer_begin_frame"),
        "stage": function_ref(RENDERER_C, "stage_prepared_quad"),
        "renderer_world": function_ref(RENDERER_C, "renderer_draw_world"),
        "gpu_submit": function_ref(GPU_TRANSPORT_C, "gpu_transport_submit_descriptors"),
        "voxel_header": source_ref(VOXEL_GPU_H, r"^ \* Register map"),
        "mesh_worker": source_ref(MESH_WORKER_H, r"Background mesh rebuilder"),
        "gen_worker": source_ref(GEN_WORKER_H, r"Background chunk generator"),
        "mesh_job": function_ref(WORLD_C, "world_run_mesh_job"),
        "stream_func": function_ref(WORLD_C, "world_stream_around"),
        "set_block": function_ref(WORLD_C, "world_set_block_locked"),
        "player_update": function_ref(PLAYER_C, "player_update"),
        "renderer_mesh_read": source_ref(RENDERER_C, r"atomic_load_explicit\(&chunk->live_mesh"),
        "renderer_submit": source_ref(RENDERER_C, r"gpu_transport_submit_descriptors\(ctx->transport"),
    }

    return f"""# HPS Game Logic Technical Breakdown

This document covers the HPS-side C game logic only. It is generated from source names and line references in `sw/`; it does not infer behavior from a demo, logs, or Quartus reports.

## Source Scope

{source_inventory()}

Key entry point: `sw/game.c::main()` starts at {refs["main"]}. Core constants for frame pacing, physics, reach distance, survival timers, and UI sizing are defined near {refs["defaults"]}; notable timing constants include `DEFAULT_TARGET_FPS` at {refs["target_fps"]}, `PHYSICS_HZ`/`PHYSICS_DT` at {refs["physics_hz"]}, `MAX_FRAME_DT` at {refs["max_frame_dt"]}, block reach at {refs["reach"]}, `ENVIRONMENT_TICK_INTERVAL` at {refs["env_tick"]}, and `REDSTONE_TICK_INTERVAL` at {refs["redstone_tick"]}.

## HPS Ownership Model

The HPS owns game state, world mutation, chunk streaming decisions, player/camera movement, input modality, inventory/crafting/furnace logic, command handling, mesh generation, descriptor generation, and the submission schedule. The FPGA owns rasterization, on-chip color/Z band caches, external SDRAM frame storage, and VGA scanout once descriptors and control registers cross the device boundary.

| Owned State | Main C Type / File | Mutated By | Read/Consumed By |
|---|---|---|---|
| Player body/mode/velocity/water state | `Player` in `sw/player_physics.h` at {refs["player_struct"]} | `player_update()` at {refs["player_update"]}, mode toggles in `game.c` | Camera sync, hazard checks, pressure plates, UI/HUD |
| Camera pose/projection | `Camera` in `sw/renderer.h` | Mouse/key look and FOV settings in `game.c` | Renderer projection and targeting |
| World/chunk storage | `VoxelWorld` at {refs["world_struct"]}, `Chunk` at {refs["chunk_struct"]} | `world_stream_around()`, `world_set_block*()`, sim ticks | Physics collision, block targeting, renderer, workers |
| Immutable render meshes | `ChunkMesh` at {refs["chunkmesh"]} | mesh rebuild/publish path | `renderer_draw_world()` reads `live_mesh` lock-free at {refs["renderer_mesh_read"]} |
| Input modes/events | `InputState` at {refs["input_state"]} | `input_update()` and consume helpers | Game loop, chat, pause, inventory, movement |
| Survival inventory/crafting | `SurvivalInventory` at {refs["inventory"]} | Inventory clicks, pickups, drops, crafting, furnace | HUD, hand rendering, placement/break speed |
| Item entities | `ItemEntityPool` in `sw/game_items.h` at {refs["items_pool"]} | block drops, tosses, pickup simulation | renderer and pressure plate triggers |
| Block registry and traits | `BlockID`, `BlockDescriptor` in `sw/block_types.h` at {refs["blocks"]} | initialized once by `init_block_types()` | world, physics, renderer, inventory, commands |
| Render descriptors | `RenderContext` / `quad_desc` / `quad_desc_uv` | `stage_prepared_quad()` at {refs["stage"]} | `gpu_transport_submit_descriptors()` at {refs["gpu_submit"]} |

## Startup And World Selection

`main()` pins the main thread, creates the renderer, initializes input/chat/pause, initializes block descriptors, and initializes the world object at {refs["startup"]}. It then enters the home-menu path, resets menu/session state, initializes inventory and item drops, and calls `run_home_menu()` at {refs["home"]}. The selected world supplies `name`, `path`, `seed`, `stone_tries_per_chunk`, and `desert_lava_pools_enabled` through `SelectedWorld`.

After selection, `game.c` creates a `Player`, builds a `Camera` from the player's eye height and selected FOV, and calls `world_init_infinite_procedural()` at {refs["world_init"]}. That world initializer sets seed/options, render/load radii, persistence metadata, and streams the initial window. The main loop then starts the mesh and generation workers at {refs["worker_start"]}; both workers can be disabled by environment variables according to their headers at {refs["mesh_worker"]} and {refs["gen_worker"]}.

## Per-Frame Order

The frame loop starts at {refs["loop"]}. Its ordering is important because the HPS is the authoritative game simulator:

1. Compute `frame_dt`, clamp it by `MAX_FRAME_DT`, and accumulate world time, physics time, environment time, and redstone time.
2. Run fluid/gravity environment simulation with `world_water_tick()` when its accumulator reaches `ENVIRONMENT_TICK_INTERVAL` at {refs["env_loop"]}.
3. Run one or more `world_update_redstone()` steps while the redstone accumulator is at least `REDSTONE_TICK_INTERVAL` at {refs["redstone_loop"]}.
4. Run `world_update_falling_blocks(&world, frame_dt)` every frame at {refs["falling_loop"]}.
5. Poll input with `input_update()`, tick chat timers, damage flash, and creative flight double-tap state at {refs["input"]}.
6. Resolve modal ownership: pause menu, inventory, furnace UI, chat text mode, debug HUD, and pointer capture. Pointer capture is explicitly disabled while pause/chat/inventory/furnace owns input at {refs["pointer"]}.
7. Apply look input to camera yaw/pitch, or to inventory/furnace cursor if an inventory-like UI is open at {refs["look"]}.
8. Convert WASD relative to camera yaw into a normalized wish direction at {refs["wish"]}.
9. Run fixed-step `player_update()` while `physics_accumulator >= PHYSICS_DT`; survival fall damage is computed around the fixed-step loop at {refs["physics"]}.
10. Sync camera position from player eye height, then call `world_stream_around()` using player position at {refs["stream"]}.
11. Run survival hazards and recovery: underwater air/drowning, cactus damage, lava damage, food-to-health regeneration, item entity pickup, pressure plates, and furnace timers beginning around {refs["hazards"]}.
12. Process one of the active interaction modes: inventory click grid, furnace click grid, or normal gameplay block/item interactions beginning around {refs["interactions"]}.
13. Run deferred lighting if safe, drain pending generated chunks, and drain dirty mesh work at {refs["workers"]}.
14. Re-sample mouse motion just before rendering, set renderer camera, draw sky/world/entities/UI, and call `renderer_end_frame()` at {refs["render"]} and {refs["render_end"]}.
15. Reap retired chunk meshes after render, accumulate performance counters, update FPS text, and sleep for the remaining frame budget at {refs["sleep"]}.

## Input And Modal Rules

The game loop gates input by modality instead of letting every subsystem consume the same click/key events. Pause owns ESC/menu selection and closes inventory/furnace first. Chat owns text input and command submission through `execute_chat_command()` at {refs["chat_enter"]}. Inventory and furnace own cursor deltas and left/right clicks. Normal movement, block breaking, placement, mode toggling, hotbar changes, and item drops are only active when pause/chat/inventory/furnace are not open.

The loop deliberately samples mouse twice: once before simulation for movement/look and again after streaming/lighting/mesh work but before render. The late re-sample is source-commented as reducing input-to-display gap on frames with nontrivial update work; this is a runtime behavior in `game.c`, not an FPGA timing claim.

## Player, Camera, And Survival Physics

`PlayerMode` in `sw/player_physics.h` defines survival, creative, and spectator modes. `player_update()` at {refs["player_update"]} applies horizontal target velocity, sprint scaling, water drag/flow push, gravity, jump/fly controls, crouch eye height, collision resolution, and spectator no-collision motion.

Survival-specific logic outside `player_update()` handles health, food, air, fall damage, cactus damage, lava damage, item pickup, and furnace progress. Creative and spectator reset or bypass survival hazards where the main loop checks `player.mode`. Creative flight is toggled by double-tapping jump; spectator always flies.

## World, Chunk, And Mesh Data Model

`VoxelWorld` owns a streaming chunk window, chunk lookup, render/load radii, persistence path, dirty flags, light/redstone/falling-block state, worker flags, and `world_mu` at {refs["world_struct"]}. Each `Chunk` stores block IDs, sky light, block light, water levels, redstone metadata, scratch face buffers, and atomic `live_mesh`/`retired_mesh` pointers. `ChunkMesh` is explicitly documented as an immutable snapshot published through atomic exchange and read lock-free by the renderer.

Block mutation through `world_set_block_locked()` at {refs["set_block"]} updates the chunk cell, marks the chunk modified, increments generation, maintains fluid/redstone metadata, marks chunk/neighbors dirty, updates sky/block light, updates redstone component counts, clears unsupported vertical plants, sets redstone dirty if needed, and sets `world->meshes_dirty`.

## Streaming, Async Generation, And Mesh Rebuild

`world_stream_around()` at {refs["stream_func"]} locks `world_mu`, recenters the loaded window, records lock-wait/body timing, and returns failure to the main loop if streaming cannot complete. Async chunk generation is described in `gen_worker.h`: `gen_worker_drain_pending()` scans LOADING chunks once per frame, queues jobs, and stale jobs are discarded by generation mismatch.

Mesh rebuilds are described in `mesh_worker.h`: `mesh_worker_drain_dirty()` queues dirty chunks once per frame or falls back to synchronous rebuild when the worker is disabled. `world_run_mesh_job()` at {refs["mesh_job"]} snapshots the target chunk and four cardinal neighbors under lock, performs heavy mesh build outside the lock, then re-locks to revalidate generation and publish the mesh. The renderer reads `chunk->live_mesh` with acquire semantics at {refs["renderer_mesh_read"]}; `mesh_worker_reap_retired()` is called after render, so replaced snapshots are freed only after the frame draw pass is done.

## Environment Simulation

The HPS runs all world simulation:

- Fluids and gravity blocks: `world_water_tick()` runs at a slower fixed interval; `world.h` documents water/lava source spread, flow evaporation, and smooth falling sand/gravel entities.
- Redstone: `world_update_redstone()` runs on 0.1-second ticks, with button pulses, repeater delay state, wires/torches/comparators, levers, and pressure plates.
- Falling blocks: `world_update_falling_blocks()` advances smooth falling entities every frame.
- Lighting: block edits update affected light immediately, while full deferred lighting rebuild is gated by player speed and recent stream body time.

## Targeting, Breaking, Placing, And Interactions

Block targeting starts with camera direction and `trace_target_block()` at {function_ref(GAME_C, "trace_target_block")}. The tracer steps by `BLOCK_TRACE_STEP`, treats fluids as pass-through for ordinary block targeting, and uses special bounds for non-cube blocks such as doors, torches, flat redstone-like devices, flowers, mushrooms, cactus, and sugar cane.

Creative breaking can repeat while the break input is held. Survival breaking tracks a stable `BlockTarget`, looks up `block_break_seconds()`, adjusts duration through `item_break_seconds()` for held tools, and only breaks after `break_timer >= break_duration`. Block drops are spawned through `item_entity_spawn_block_drop()` and special cascades handle vertical plants.

Placement first gives targeted interactive blocks a chance to consume the click: buttons, levers, repeaters, doors, crafting tables, and furnaces. If the held survival item is food, bucket, or placeable block, the matching item path runs. Successful block edits flow into `world_set_block*()`, which marks mesh/light/redstone consequences as described above.

## Inventory, Crafting, Furnaces, And Items

`SurvivalInventory` stores 9 hotbar slots, 27 main slots, a craft grid, craft output, and cursor stack. Inventory UI hit testing and click behavior are local to `game.c`; actual stack operations live in `sw/inventory.c`. Crafting table use changes the craft grid dimension to 3x3, while player inventory uses 2x2.

`FurnaceState` is game-local state keyed by furnace block position. Furnaces hold input/fuel/output stacks, a smelting flag, timer, and smelt output. `furnace_states_update()` advances active furnace timers once per frame while survival gameplay is unpaused. Breaking a furnace drops its contents.

`ItemEntityPool` stores up to `ITEM_ENTITY_MAX` active item entities. Survival block drops, tosses, and item pickup update this pool; `item_entities_draw()` submits billboard/tile quads through the same renderer path as other UI/world geometry.

## Chat And Commands

`command_parser.h` defines command kinds for `/time`, `/gamemode`, `/kill`, `/help`, `/physics`, `/setblock`, `/fill`, `/give`, and `/items`. `execute_chat_command()` dispatches parsed commands to game/world/player/inventory state. Commands are still HPS-side mutations; `/setblock` and `/fill` eventually call world block mutation and therefore mark meshes/light/redstone dirty like direct player edits.

## Renderer Handoff To FPGA

The HPS renderer reads `ChunkMesh` snapshots, projects visible faces, emits sky/world/entity/UI quads, and packs descriptors. `renderer_begin_frame()` at {refs["renderer_begin"]} clears per-frame state and starts bin-during-emit. `renderer_draw_world()` at {refs["renderer_world"]} selects loaded chunks with live meshes, culls by render distance/frustum, draws opaque geometry first, emits falling blocks, then sorts translucent faces back-to-front.

`stage_prepared_quad()` at {refs["stage"]} snaps screen-space vertices to Q24.8, computes integer bounding boxes, edge coefficients, depth gradients, and optional UV planes. `renderer_end_frame()` submits packed descriptors through `gpu_transport_submit_descriptors()` at {refs["renderer_submit"]}. The register/descriptor ABI is documented in `sw/voxel_gpu.h` at {refs["voxel_header"]}.

## Shutdown And Persistence

On exit, the loop stops generation before mesh rebuilding, flushes modified chunks with `world_flush()`, frees world state, optionally returns to the home menu by reinitializing the world, then shuts down input and renderer at {refs["shutdown"]}. The stop order is source-commented: generation can enqueue mesh-dirty work during finalize, so it is stopped first.

## Function Source Map

{function_inventory()}

## Source-Grounded Limits

- This breakdown is based on C headers/source and does not claim observed hardware performance.
- Exact frame time, stream wait, mesh cost, and FPS values require runtime logs from the game loop.
- FPGA raster timing, Fmax, resource utilization, and timing closure require RTL simulation artifacts or Quartus reports; they are outside HPS game-logic evidence.
- The renderer handoff is documented through descriptor generation/submission, not by claiming what a particular monitor displayed.
"""


def hps_game_loop_flow() -> str:
    return mermaid_header() + """flowchart TB
subgraph Init["Startup / Session Setup"]
  boot["main(): thread pin, renderer_init, input/chat/pause init"]:::hps
  menu["run_home_menu(): SelectedWorld path/seed/options"]:::hps
  worldinit["world_init_infinite_procedural(): persistence + initial chunk window"]:::hps
  workers["mesh_worker_start + gen_worker_start"]:::hps
end
subgraph Frame["Per-Frame HPS Game Loop"]
  time["dt clamp + accumulators<br/>world, physics, environment, redstone"]:::ctrl
  env["world_water_tick() when 0.75s accumulator fires"]:::hps
  red["world_update_redstone() while 0.1s ticks pending"]:::hps
  fall["world_update_falling_blocks(frame_dt)"]:::hps
  input["input_update() + chat/damage/flight timers"]:::hps
  modes["pause / chat / inventory / furnace ownership gates"]:::ctrl
  look["camera look or inventory cursor update"]:::hps
  wish["WASD -> normalized camera-relative wish vector"]:::hps
  phys["fixed-step player_update(PHYSICS_DT)<br/>fall damage tracking"]:::hps
  stream["world_stream_around(player.x,z)"]:::hps
  survival["air, drowning, cactus/lava, food regen,<br/>items, pressure plates, furnaces"]:::hps
  interact["inventory/furnace clicks or normal block/item interactions"]:::hps
  drains["deferred lighting + gen_worker_drain_pending + mesh_worker_drain_dirty"]:::hps
  render["renderer_begin_frame -> draw sky/world/entities/UI -> renderer_end_frame"]:::hps
  cleanup["mesh_worker_reap_retired + perf counters + frame sleep"]:::hps
end
subgraph Exit["Exit / Return To Menu"]
  stop["gen_worker_stop -> mesh_worker_stop -> world_flush -> world_free"]:::hps
end
boot --> menu --> worldinit --> workers --> time
time --> env --> red --> fall --> input --> modes --> look --> wish --> phys --> stream --> survival --> interact --> drains --> render --> cleanup --> time
cleanup --> stop
""" + class_defs()


def hps_world_streaming_mesh_flow() -> str:
    return mermaid_header() + """flowchart LR
player["Player position<br/>camera/world center"]:::hps
stream["world_stream_around() under world_mu"]:::hps
window["Chunk window<br/>LOADED / LOADING / evicted slots"]:::mem
genq["gen_worker_drain_pending()<br/>queue LOADING chunks"]:::hps
genjob["world_async_chunk_gen_offline()<br/>terrain/load/sky light without world_mu"]:::hps
finalize["world_finalize_async_chunk_load()<br/>generation check + install result"]:::hps
dirty["mark self/neighbors mesh dirty<br/>lighting/env/redstone state updated"]:::ctrl
meshq["mesh_worker_drain_dirty()<br/>queue dirty chunks or sync rebuild"]:::hps
snapshot["world_run_mesh_job phase 1<br/>snapshot self + 4 neighbors under lock"]:::hps
build["mesh build outside lock<br/>greedy/non-cube/translucent partitions"]:::hps
publish["re-lock, generation check,<br/>publish ChunkMesh via atomic live_mesh"]:::hps
render["renderer_draw_world()<br/>atomic_load live_mesh lock-free"]:::hps
retire["mesh_worker_reap_retired()<br/>free replaced snapshots after render"]:::hps
player --> stream --> window
window --> genq --> genjob --> finalize --> dirty
dirty --> meshq --> snapshot --> build --> publish --> render --> retire
stream --> dirty
""" + class_defs()


def hps_interaction_logic_flow() -> str:
    return mermaid_header() + """flowchart TB
input["Normal gameplay click/key events<br/>only when pause/chat/inventory/furnace closed"]:::hps
target["trace_target_block() / trace_target_fluid_source()<br/>camera ray + non-cube bounds"]:::hps
mode{"Player mode"}:::ctrl
creative["Creative: instant/repeat break,<br/>hotbar block placement"]:::hps
survival["Survival: inventory hotbar stack,<br/>tool speed, food/bucket/placeable item paths"]:::hps
interact["Priority interactions<br/>button, lever, repeater, door,<br/>crafting table, furnace"]:::hps
breaktimer["Survival break timer<br/>same target + block/item break duration"]:::ctrl
drops["Block drops / furnace contents / plant cascade<br/>ItemEntityPool"]:::hps
place["try_place_targeted_block/door/bucket<br/>support + replacement checks"]:::hps
worldset["world_set_block*()<br/>chunk cell, generation, metadata"]:::hps
sidefx["mark mesh/env dirty<br/>update light/redstone counts<br/>clear unsupported plants"]:::ctrl
workers["mesh/gen drain later in same frame"]:::hps
render["renderer reads new live_mesh after rebuild publish"]:::hps
input --> target --> mode
mode --> creative
mode --> survival
creative --> interact
creative --> place
creative --> worldset
survival --> interact
survival --> breaktimer --> drops --> worldset
survival --> place --> worldset
interact --> worldset
worldset --> sidefx --> workers --> render
""" + class_defs()


def main() -> int:
    ensure_dirs()
    outputs = {
        DOCS / "hps_game_logic_breakdown.md": breakdown_md(),
        DIAGRAMS / "hps_game_loop_flow.mmd": hps_game_loop_flow(),
        DIAGRAMS / "hps_world_streaming_mesh_flow.mmd": hps_world_streaming_mesh_flow(),
        DIAGRAMS / "hps_interaction_logic_flow.mmd": hps_interaction_logic_flow(),
    }
    for path, text in outputs.items():
        write(path, text)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
