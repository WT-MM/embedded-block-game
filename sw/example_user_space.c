#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

// Include the shared header file you provided
#include "voxel_gpu.h" 

// --- Helper Functions for IOCTLs ---

// 1. Clear the frame (Z-buffer and screen buffer)
int gpu_clear_frame(int fd) {
    if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME) < 0) {
        fprintf(stderr, "Failed to clear frame: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

// 2. Flip the front/back buffers (usually done after pushing all quads)
int gpu_flip(int fd) {
    if (ioctl(fd, VOXEL_IOC_FLIP) < 0) {
        fprintf(stderr, "Failed to flip buffers: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

// 3. Get the hardware status (useful for polling before sending more data)
int gpu_get_status(int fd, struct voxel_status *status) {
    if (ioctl(fd, VOXEL_IOC_GET_STATUS, status) < 0) {
        fprintf(stderr, "Failed to get status: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

// 4. Set a specific color in the palette
int gpu_set_palette(int fd, uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    struct voxel_palette_entry entry = {
        .index = index,
        .r = r,
        .g = g,
        .b = b
    };
    
    if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &entry) < 0) {
        fprintf(stderr, "Failed to set palette index %d: %s\n", index, strerror(errno));
        return -1;
    }
    return 0;
}

// 5. Get the total frames rendered by the FPGA
int gpu_get_frame_count(int fd, uint32_t *frame_count) {
    if (ioctl(fd, VOXEL_IOC_GET_FRAME_COUNT, frame_count) < 0) {
        fprintf(stderr, "Failed to get frame count: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}


// --- Main Application Loop ---

int main() {
    // 1. Open the device exposed by your kernel driver
    int fd = open("/dev/voxel_gpu", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error opening /dev/voxel_gpu: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Successfully connected to FPGA GPU.\n");

    // 2. Initialize Palette (Example: set index 1 to Red)
    gpu_set_palette(fd, 1, 255, 0, 0);

    // 3. The Render Loop
    for (int frame = 0; frame < 60; frame++) { // Run for 60 frames
        
        // A. Clear the previous frame data
        gpu_clear_frame(fd);

        // B. Check if FIFO has room (Polling since the driver doesn't use interrupts)
        struct voxel_status stat;
        do {
            gpu_get_status(fd, &stat);
            // Ideally, yield or sleep briefly here if busy to avoid 100% CPU lock
        } while (stat.fifo_full || stat.busy);

        // C. Stream your geometry data into the FIFO using standard write()
        // (Assuming you have an array of your RenderQuad structs here)
        /*
         * RenderQuad my_quads[10];
         * // ... populate quads ...
         * ssize_t bytes_written = write(fd, my_quads, sizeof(my_quads));
         */

        // D. Tell the FPGA to swap buffers so the frame becomes visible
        gpu_flip(fd);
    }

    // 4. Check hardware stats
    uint32_t total_frames = 0;
    if (gpu_get_frame_count(fd, &total_frames) == 0) {
        printf("Total frames rendered by hardware: %u\n", total_frames);
    }

    // 5. Cleanup
    close(fd);
    return EXIT_SUCCESS;
}