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

void player_update(Player *p, VoxelWorld *world, float wish_dir_x, float wish_dir_z,
                   bool jump, bool shift, bool sprint, float dt) {
    bool apply_gravity   = (p->mode == PLAYER_MODE_SURVIVAL);
    bool apply_collision = (p->mode != PLAYER_MODE_SPECTATOR);

    /* Horizontal slewing (same in all modes). Sprint scales the target
     * speed — not the current velocity — so acceleration feels natural. */
    float horiz_speed = sprint ? (MAX_SPEED * SPRINT_MULTIPLIER) : MAX_SPEED;
    float target_vx = wish_dir_x * horiz_speed;
    float target_vz = wish_dir_z * horiz_speed;
    float accel_rate = (wish_dir_x != 0.0f || wish_dir_z != 0.0f) ? ACCELERATION : FRICTION;

    p->vx = approach(p->vx, target_vx, accel_rate * dt);
    p->vz = approach(p->vz, target_vz, accel_rate * dt);

    /* Vertical control: gravity + jump in survival, direct fly in creative/spectator.
     * In flying modes jump=ascend, shift=descend, and eye-height crouch is disabled. */
    if (apply_gravity) {
        p->vy -= GRAVITY * dt;
        if (jump && p->is_grounded) {
            p->vy = JUMP_VELOCITY;
            p->is_grounded = false;
        }
        p->is_shifting = shift;
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
