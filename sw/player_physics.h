#ifndef PLAYER_PHYSICS_H
#define PLAYER_PHYSICS_H

#include <stdbool.h>
#include "world.h"

#define PLAYER_WIDTH       0.5f
#define PLAYER_DEPTH       0.5f
#define PLAYER_HEIGHT      1.8f

#define EYE_HEIGHT_NORMAL  1.5f
#define EYE_HEIGHT_SHIFT   1.3f

#define GRAVITY            9.8f  /* blocks per second squared */
#define JUMP_VELOCITY      4.85f /* derived from ~1.2 block max jump height */
#define MAX_SPEED          4.0f  /* blocks per second */
#define SPRINT_MULTIPLIER  1.6f  /* scales horizontal target speed while sprinting */
#define ACCELERATION       15.0f /* rate of slewing/acceleration */
#define FRICTION           10.0f /* deceleration when no input is applied */
#define FLY_SPEED          6.0f  /* vertical speed in creative/spectator */

/* Water physics: gravity is dampened, terminal sink rate is clamped, and
 * a held jump key lets the player swim up. Horizontal speed is also
 * scaled down so movement feels heavy. */
#define WATER_GRAVITY_FACTOR     0.30f
#define WATER_SINK_TERMINAL      -1.4f
#define WATER_SWIM_UP_VELOCITY   3.2f
#define WATER_HORIZONTAL_DRAG    0.55f
#define WATER_FLOW_PUSH_SPEED    1.35f

/* How many physics ticks between full water AABB re-checks when the player
 * is grounded and stationary. 3 ticks @ 60 Hz = ~20 Hz re-check rate.
 * The re-check triggers immediately whenever vy is non-zero. */
#define WATER_CHECK_INTERVAL     3

typedef enum {
    PLAYER_MODE_SURVIVAL  = 0, /* gravity + collision */
    PLAYER_MODE_CREATIVE  = 1, /* fly, no gravity, collision */
    PLAYER_MODE_SPECTATOR = 2, /* fly, no gravity, no collision */
    PLAYER_MODE_COUNT     = 3,
} PlayerMode;

typedef struct {
    PlayerMode mode;
    float x, y, z;        /* position of the player's feet */
    float vx, vy, vz;     /* current velocity */
    bool is_grounded;
    bool is_shifting;
    float current_eye_y;  /* dynamic eye offset */
    /* Cached water state. Refreshed every WATER_CHECK_INTERVAL physics ticks
     * (or immediately whenever the player moves vertically). Running the full
     * AABB voxel scan at 60 Hz was the dominant cost in upd_phys at idle. */
    bool is_in_water;
    int  water_check_countdown;
    float water_flow_x, water_flow_z;
} Player;

void player_init(Player *p, float start_x, float start_y, float start_z);
void player_update(Player *p, VoxelWorld *world, float wish_dir_x, float wish_dir_z,
                   bool jump, bool up_held, bool shift, bool sprint, float dt);
float player_get_eye_height(const Player *p);
void player_cycle_mode(Player *p);
const char *player_mode_name(PlayerMode mode);

#endif
