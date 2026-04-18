#include <stdio.h>
#include <math.h>

// Screen dimensions
#define SCREEN_WIDTH 320.0f
#define SCREEN_HEIGHT 240.0f

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float x, y;
    int visible; // 1 if in front of camera, 0 if behind
} Vec2;

typedef struct {
    Vec3 position; // Minimum x, y, z corner of the block
} Block;

typedef struct {
    Vec3 position;
    float pitch; // Up/down rotation in radians
    float yaw;   // Left/right rotation in radians
    float depth; // Focal length / projection plane distance
} Camera;

// Function to transform a single 3D point into 2D screen coordinates
Vec2 project_point(Vec3 point, Camera cam) {
    Vec2 result;
    result.visible = 0;

    // 1. Translate point relative to camera (View Matrix translation)
    float dx = point.x - cam.position.x;
    float dy = point.y - cam.position.y;
    float dz = point.z - cam.position.z;

    // Precompute sine and cosine for rotations
    float cos_yaw = cosf(cam.yaw);
    float sin_yaw = sinf(cam.yaw);
    float cos_pitch = cosf(cam.pitch);
    float sin_pitch = sinf(cam.pitch);

    // 2. Apply Yaw (Rotation around Y axis)
    float x_rot_yaw = dx * cos_yaw - dz * sin_yaw;
    float z_rot_yaw = dx * sin_yaw + dz * cos_yaw;
    float y_rot_yaw = dy;

    // 3. Apply Pitch (Rotation around X axis)
    float x_cam = x_rot_yaw;
    float y_cam = y_rot_yaw * cos_pitch - z_rot_yaw * sin_pitch;
    float z_cam = y_rot_yaw * sin_pitch + z_rot_yaw * cos_pitch;

    // 4. Perspective Projection
    // Only project if the point is in front of the camera (z > 0)
    // To prevent division by zero or negative projection, we check z_cam.
    // (Note: Proper 3D engines clip polygons that intersect the near plane, 
    // but we are just calculating corner coordinates here).
    if (z_cam > 0.1f) { 
        result.visible = 1;
        
        // Perspective divide and scale by camera depth (focal length)
        float projected_x = (x_cam / z_cam) * cam.depth;
        float projected_y = (y_cam / z_cam) * cam.depth;

        // Map to screen coordinates (centering the origin to the middle of the screen)
        result.x = projected_x + (SCREEN_WIDTH / 2.0f);
        result.y = projected_y + (SCREEN_HEIGHT / 2.0f);
    }

    return result;
}

// Function to calculate all 8 corners for an array of blocks
void calculate_block_corners(Block* blocks, int num_blocks, Camera cam) {
    // The 8 local offsets for the corners of a 1x1x1 block
    Vec3 offsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, // Front face
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}  // Back face
    };

    for (int i = 0; i < num_blocks; i++) {
        printf("Block %d at (%.1f, %.1f, %.1f):\n", 
               i, blocks[i].position.x, blocks[i].position.y, blocks[i].position.z);

        for (int j = 0; j < 8; j++) {
            // Calculate world position of this corner
            Vec3 corner_world = {
                blocks[i].position.x + offsets[j].x,
                blocks[i].position.y + offsets[j].y,
                blocks[i].position.z + offsets[j].z
            };

            // Project the corner to the screen
            Vec2 screen_corner = project_point(corner_world, cam);

            if (screen_corner.visible) {
                printf("  Corner %d: 2D Screen Pos = (%.1f, %.1f)\n", 
                       j, screen_corner.x, screen_corner.y);
            } else {
                printf("  Corner %d: [Behind Camera]\n", j);
            }
        }
        printf("\n");
    }
}

int main() {
    // Setup camera
    Camera cam;
    cam.position = (Vec3){0.0f, 1.5f, -3.0f}; // Standing slightly back and up
    cam.pitch = 0.2f;  // Looking slightly down
    cam.yaw = 0.0f;    // Looking straight ahead down the Z axis
    cam.depth = 250.0f; // Represents FOV. Higher = more zoomed in.

    // Define a couple of blocks in the world
    Block blocks[2];
    blocks[0].position = (Vec3){-1.0f, 0.0f, 2.0f}; 
    blocks[1].position = (Vec3){2.0f, 0.0f, 4.0f};

    // Calculate and print the corners
    calculate_block_corners(blocks, 2, cam);

    return 0;
}