#ifndef BLOCK_TYPES_H
#define BLOCK_TYPES_H

#include <stdint.h>

#define TEXTURE_WIDTH 16
#define TEXTURE_HEIGHT 16

// 1. Define the basic color structure
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGBColor;

// 2. Define the Texture structure (16x16 array of RGB values)
typedef struct {
    RGBColor pixels[TEXTURE_HEIGHT][TEXTURE_WIDTH];
} Texture;

// 3. Define Face Indices for easy mapping
typedef enum {
    FACE_TOP = 0,
    FACE_BOTTOM,
    FACE_LEFT,
    FACE_RIGHT,
    FACE_FRONT,
    FACE_BACK,
    NUM_FACES
} BlockFace;

// 4. Define the Block Types (Extensible: just add more here before NUM_BLOCK_TYPES)
typedef enum {
    BLOCK_AIR = 0, // Always good to have an empty block type
    BLOCK_GRASS,
    BLOCK_WOOD,
    BLOCK_DIRT,
    BLOCK_STONE,
    NUM_BLOCK_TYPES
} BlockID;

// 5. Define the Block Descriptor
// We use pointers to Textures so multiple faces or blocks can share the same texture 
// (e.g., Grass sides and Dirt use the same memory).
typedef struct {
    BlockID id;
    const char* name;
    Texture* face_textures[NUM_FACES]; // Array of 6 texture pointers
} BlockDescriptor;

// Global Registry Arrays
extern BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

// Function Prototypes
void init_block_types();

#endif // BLOCK_TYPES_H