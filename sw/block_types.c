#include "block_types.h"
#include <string.h>

// The global registry that holds our block definitions
BlockDescriptor BlockRegistry[NUM_BLOCK_TYPES];

// A small internal texture registry to hold our generated textures
// Extensible: Add more texture slots as you add things like "stone_bricks", "leaves", etc.
typedef enum {
    TEX_GREEN_GRASS = 0,
    TEX_DIRT_BROWN,
    TEX_WOOD_DARK_BROWN,
    TEX_STONE_GREY,
    NUM_TEXTURES
} TextureID;

Texture TextureRegistry[NUM_TEXTURES];

// Helper function to fill a 16x16 texture with a solid RGB color
static void generate_solid_texture(Texture* tex, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = 0; y < TEXTURE_HEIGHT; y++) {
        for (int x = 0; x < TEXTURE_WIDTH; x++) {
            tex->pixels[y][x].r = r;
            tex->pixels[y][x].g = g;
            tex->pixels[y][x].b = b;
        }
    }
}

// Helper function to assign the exact same texture to all 6 faces of a block
static void set_all_faces(BlockDescriptor* block, Texture* tex) {
    for (int i = 0; i < NUM_FACES; i++) {
        block->face_textures[i] = tex;
    }
}

void init_block_types() {
    // 1. Generate the actual 16x16 Texture Data
    generate_solid_texture(&TextureRegistry[TEX_GREEN_GRASS], 34, 139, 34);     // Forest Green
    generate_solid_texture(&TextureRegistry[TEX_DIRT_BROWN], 139, 69, 19);      // Light/Dirt Brown
    generate_solid_texture(&TextureRegistry[TEX_WOOD_DARK_BROWN], 101, 67, 33); // Dark Wood Brown
    generate_solid_texture(&TextureRegistry[TEX_STONE_GREY], 128, 128, 128);    // Neutral Grey

    // 2. Initialize Air (Invisible, no textures)
    BlockRegistry[BLOCK_AIR].id = BLOCK_AIR;
    BlockRegistry[BLOCK_AIR].name = "Air";
    set_all_faces(&BlockRegistry[BLOCK_AIR], NULL);

    // 3. Initialize Dirt
    BlockRegistry[BLOCK_DIRT].id = BLOCK_DIRT;
    BlockRegistry[BLOCK_DIRT].name = "Dirt";
    set_all_faces(&BlockRegistry[BLOCK_DIRT], &TextureRegistry[TEX_DIRT_BROWN]);

    // 4. Initialize Wood
    BlockRegistry[BLOCK_WOOD].id = BLOCK_WOOD;
    BlockRegistry[BLOCK_WOOD].name = "Wood";
    set_all_faces(&BlockRegistry[BLOCK_WOOD], &TextureRegistry[TEX_WOOD_DARK_BROWN]);

    // 5. Initialize Stone
    BlockRegistry[BLOCK_STONE].id = BLOCK_STONE;
    BlockRegistry[BLOCK_STONE].name = "Stone";
    set_all_faces(&BlockRegistry[BLOCK_STONE], &TextureRegistry[TEX_STONE_GREY]);

    // 6. Initialize Grass (Special case: different textures on different faces)
    BlockRegistry[BLOCK_GRASS].id = BLOCK_GRASS;
    BlockRegistry[BLOCK_GRASS].name = "Grass";
    
    // Top is green
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_TOP] = &TextureRegistry[TEX_GREEN_GRASS];
    
    // Bottom is dirt
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_BOTTOM] = &TextureRegistry[TEX_DIRT_BROWN];
    
    // Sides are dirt (For a true Minecraft feel, you might later make a specific "grass_side" texture)
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_FRONT] = &TextureRegistry[TEX_DIRT_BROWN];
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_BACK]  = &TextureRegistry[TEX_DIRT_BROWN];
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_LEFT]  = &TextureRegistry[TEX_DIRT_BROWN];
    BlockRegistry[BLOCK_GRASS].face_textures[FACE_RIGHT] = &TextureRegistry[TEX_DIRT_BROWN];
}