#include "player_physics.h"
#include <math.h>

static float approach(float current, float target, float delta) {
    if (current < target) {
        return (current + delta > target) ? target : current + delta;
    } else if (current > target) {
        return (current - delta < target) ? target : current - delta;
    }
    return target;
}

void player_init(Player *p, float start_x, float start_y, float start_z) {
    p->mode = PLAYER_MODE_SURVIVAL;
    p->x = start_x;
    p->y = start_y;
    p->z = start_z;
    p->vx = p->vy = p->vz = 0.0f;
    p->is_grounded = false;
    p->is_shifting = false;
    p->current_eye_y = EYE_HEIGHT_NORMAL;
    p->is_in_water = false;
    p->water_check_countdown = 0;
    p->water_flow_x = 0.0f;
    p->water_flow_z = 0.0f;
}

void player_cycle_mode(Player *p) {
    p->mode = (PlayerMode)(((int)p->mode + 1) % PLAYER_MODE_COUNT);
    /* Reset vertical velocity so the player doesn't keep falling after
     * toggling into a flying mode, or blast off after toggling out. */
    p->vy = 0.0f;
    p->is_grounded = false;
    p->is_shifting = false;
}

const char *player_mode_name(PlayerMode mode) {
    switch (mode) {
    case PLAYER_MODE_SURVIVAL:  return "survival";
    case PLAYER_MODE_CREATIVE:  return "creative";
    case PLAYER_MODE_SPECTATOR: return "spectator";
    default:                    return "unknown";
    }
}

/* Helper to check if the AABB intersects any solid voxel */
static bool check_collision(VoxelWorld *world, float px, float py, float pz) {
    int min_x = (int)floorf(px - PLAYER_WIDTH / 2.0f);
    int max_x = (int)floorf(px + PLAYER_WIDTH / 2.0f);
    int min_y = (int)floorf(py);
    int max_y = (int)floorf(py + PLAYER_HEIGHT);
    int min_z = (int)floorf(pz - PLAYER_DEPTH / 2.0f);
    int max_z = (int)floorf(pz + PLAYER_DEPTH / 2.0f);

    for (int y = min_y; y <= max_y; y++) {
        for (int z = min_z; z <= max_z; z++) {
            for (int x = min_x; x <= max_x; x++) {
                if (!block_is_passable(world_get_block(world, x, y, z))) {
                    return true;
                }
            }
        }
    }
    return false;
}

static int floor_div_i(int value, int divisor) {
    int q = value / divisor;
    int r = value % divisor;

    if (r < 0)
        q--;
    return q;
}

static int positive_mod_i(int value, int divisor) {
    int r = value % divisor;

    if (r < 0)
        r += divisor;
    return r;
}

static bool block_is_water(BlockID id) {
    return id == BLOCK_WATER || id == BLOCK_WATER_FLOW;
}

static int water_level_at(VoxelWorld *world, int wx, int wy, int wz) {
    if (wy < 0 || wy >= WORLD_CHUNK_HEIGHT)
        return 8;

    int chunk_x = floor_div_i(wx, WORLD_CHUNK_SIZE);
    int chunk_z = floor_div_i(wz, WORLD_CHUNK_SIZE);
    const Chunk *chunk = world_get_chunk(world, chunk_x, chunk_z);
    if (!chunk || !(chunk->flags & CHUNK_FLAG_LOADED))
        return 8;

    int lx = positive_mod_i(wx, WORLD_CHUNK_SIZE);
    int lz = positive_mod_i(wz, WORLD_CHUNK_SIZE);
    BlockID id = chunk->blocks[wy][lz][lx];

    if (id == BLOCK_WATER)
        return 0;
    if (id != BLOCK_WATER_FLOW)
        return 8;

    int level = chunk->water_level[wy][lz][lx];
    if (level < 1)
        level = 1;
    if (level > 7)
        level = 7;
    return level;
}

static void accumulate_water_flow(VoxelWorld *world,
                                  int wx, int wy, int wz,
                                  float *flow_x, float *flow_z) {
    int current = water_level_at(world, wx, wy, wz);
    const int sx[4] = {-1, 1,  0, 0};
    const int sz[4] = { 0, 0, -1, 1};

    if (current > 7)
        return;
    if (wy > 0 && world_get_block(world, wx, wy - 1, wz) == BLOCK_AIR)
        return;

    for (int d = 0; d < 4; d++) {
        int nx = wx + sx[d];
        int nz = wz + sz[d];
        BlockID nb = world_get_block(world, nx, wy, nz);
        if (!block_is_water(nb))
            continue;

        int neighbor = water_level_at(world, nx, wy, nz);
        float delta = (float)(neighbor - current);
        *flow_x += (float)sx[d] * delta;
        *flow_z += (float)sz[d] * delta;
    }
}

/* Any water voxel overlapping the player AABB counts as "in water" — the
 * player only needs ankle-deep contact to start swimming. While scanning,
 * derive a Minecraft-like horizontal current from the local water-level
 * gradient so streams gently carry the player downstream. */
static bool player_water_contact(VoxelWorld *world, const Player *p,
                                 float *flow_x, float *flow_z) {
    int min_x = (int)floorf(p->x - PLAYER_WIDTH / 2.0f);
    int max_x = (int)floorf(p->x + PLAYER_WIDTH / 2.0f);
    int min_y = (int)floorf(p->y);
    int max_y = (int)floorf(p->y + PLAYER_HEIGHT);
    int min_z = (int)floorf(p->z - PLAYER_DEPTH / 2.0f);
    int max_z = (int)floorf(p->z + PLAYER_DEPTH / 2.0f);
    bool touched = false;
    float fx = 0.0f;
    float fz = 0.0f;

    for (int y = min_y; y <= max_y; y++)
        for (int z = min_z; z <= max_z; z++)
            for (int x = min_x; x <= max_x; x++) {
                BlockID b = world_get_block(world, x, y, z);
                if (block_is_water(b)) {
                    touched = true;
                    accumulate_water_flow(world, x, y, z, &fx, &fz);
                }
            }

    float len_sq = fx * fx + fz * fz;
    if (len_sq > 0.0001f) {
        float inv_len = 1.0f / sqrtf(len_sq);
        fx *= inv_len;
        fz *= inv_len;
    } else {
        fx = 0.0f;
        fz = 0.0f;
    }

    if (flow_x) *flow_x = fx;
    if (flow_z) *flow_z = fz;
    return touched;
}

void player_update(Player *p, VoxelWorld *world, float wish_dir_x, float wish_dir_z,
                   bool jump, bool up_held, bool shift, bool sprint, float dt) {
    bool apply_gravity   = (p->mode == PLAYER_MODE_SURVIVAL);
    bool apply_collision = (p->mode != PLAYER_MODE_SPECTATOR);
    /* Water physics is a survival-mode feature: creative/spectator keep
     * their normal fly controls so the player can plow through water at
     * full speed when building. */
    bool in_water = false;
    if (apply_gravity && apply_collision) {
        /* Re-check immediately if the player is moving vertically (entering
         * or leaving water), or on the countdown timer otherwise. */
        bool needs_check = (p->vy != 0.0f) || (--p->water_check_countdown <= 0);
        if (needs_check) {
            p->is_in_water = player_water_contact(world, p,
                                                  &p->water_flow_x,
                                                  &p->water_flow_z);
            p->water_check_countdown = WATER_CHECK_INTERVAL;
        }
        in_water = p->is_in_water;
    }

    /* Horizontal slewing (same in all modes). Sprint scales the target
     * speed — not the current velocity — so acceleration feels natural.
     * Water drag scales the resulting target down so swimming feels heavy. */
    float horiz_speed = sprint ? (MAX_SPEED * SPRINT_MULTIPLIER) : MAX_SPEED;
    if (in_water)
        horiz_speed *= WATER_HORIZONTAL_DRAG;
    float target_vx = wish_dir_x * horiz_speed;
    float target_vz = wish_dir_z * horiz_speed;
    if (in_water) {
        target_vx += p->water_flow_x * WATER_FLOW_PUSH_SPEED;
        target_vz += p->water_flow_z * WATER_FLOW_PUSH_SPEED;
    }
    float accel_rate = (wish_dir_x != 0.0f || wish_dir_z != 0.0f) ? ACCELERATION : FRICTION;

    p->vx = approach(p->vx, target_vx, accel_rate * dt);
    p->vz = approach(p->vz, target_vz, accel_rate * dt);

    /* Vertical control: gravity + jump in survival, direct fly in creative/spectator.
     * In flying modes jump=ascend, shift=descend, and eye-height crouch is disabled.
     * Water replaces gravity with a slow sink + held-Space swim-up. */
    if (apply_gravity) {
        if (in_water) {
            p->vy -= GRAVITY * WATER_GRAVITY_FACTOR * dt;
            if (p->vy < WATER_SINK_TERMINAL)
                p->vy = WATER_SINK_TERMINAL;
            /* Held Space (not edge-triggered jump) drives the swim — this
             * keeps the player rising as long as the key is down, the way
             * Minecraft swimming works. */
            if (up_held)
                p->vy = WATER_SWIM_UP_VELOCITY;
        } else {
            p->vy -= GRAVITY * dt;
            if (jump && p->is_grounded) {
                p->vy = JUMP_VELOCITY;
                p->is_grounded = false;
            }
        }
        p->is_shifting = shift && !in_water;
    } else {
        float fly_target = 0.0f;
        if (jump)  fly_target += FLY_SPEED;
        if (shift) fly_target -= FLY_SPEED;
        p->vy = approach(p->vy, fly_target, ACCELERATION * dt);
        p->is_shifting = false;
        p->is_grounded = false;
    }

    float target_eye = p->is_shifting ? EYE_HEIGHT_SHIFT : EYE_HEIGHT_NORMAL;
    p->current_eye_y = approach(p->current_eye_y, target_eye, 5.0f * dt);

    if (apply_collision) {
        p->is_grounded = false;

        if (p->vx != 0.0f) {
            float next_x = p->x + p->vx * dt;
            if (!check_collision(world, next_x, p->y, p->z))
                p->x = next_x;
            else
                p->vx = 0.0f;
        }

        if (p->vy != 0.0f) {
            float next_y = p->y + p->vy * dt;
            if (!check_collision(world, p->x, next_y, p->z)) {
                p->y = next_y;
            } else {
                if (apply_gravity && p->vy < 0.0f)
                    p->is_grounded = true;
                p->vy = 0.0f;
                if (apply_gravity)
                    p->y = roundf(p->y);
            }
        }

        if (p->vz != 0.0f) {
            float next_z = p->z + p->vz * dt;
            if (!check_collision(world, p->x, p->y, next_z))
                p->z = next_z;
            else
                p->vz = 0.0f;
        }
    } else {
        /* Spectator: free-fly, no collision */
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
    }
}

float player_get_eye_height(const Player *p) {
    return p->y + p->current_eye_y;
}
