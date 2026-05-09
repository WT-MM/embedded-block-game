/*
 * fpga_band_offset_test.c — visual band-offset diagnostic.
 *
 * Renders 8 horizontal bands, each 60 rows tall, each a distinct flat color:
 *
 *     band 0 (rows   0..59 ) red
 *     band 1 (rows  60..119) green
 *     band 2 (rows 120..179) blue
 *     band 3 (rows 180..239) yellow
 *     band 4 (rows 240..299) magenta
 *     band 5 (rows 300..359) cyan
 *     band 6 (rows 360..419) white
 *     band 7 (rows 420..479) orange
 *
 * Each frame: CLEAR + 8 BEGIN_BAND/quad/END_BAND + FLIP. Frames repeat for
 * a configurable count or until Ctrl-C.
 *
 * Connect a VGA monitor and look:
 *   * Correct bands: 8 horizontal stripes in the order above, no duplicates.
 *   * Duplication bug: any color appears in more than one band, or bands appear
 *     in the wrong vertical order.
 *
 * (We can't read back FPGA SDRAM via /dev/mem on this hardware — DRAM_DQ pins
 *  are wired only to the voxel GPU's internal controller, so the qsys
 *  fpga_sdram Avalon slave has no physical SDRAM behind it.)
 *
 * Run: ./tests/fpga_band_offset_test [num_frames]   (default: loop until Ctrl-C)
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "voxel_gpu.h"

#define DEV_PATH "/dev/voxel_gpu"

struct band_color { uint8_t r, g, b; const char *name; };

static const struct band_color BAND_COLORS[VOXEL_BAND_COUNT] = {
    { 0xFF, 0x00, 0x00, "red"     },
    { 0x00, 0xFF, 0x00, "green"   },
    { 0x00, 0x80, 0xFF, "blue"    },
    { 0xFF, 0xFF, 0x00, "yellow"  },
    { 0xFF, 0x00, 0xFF, "magenta" },
    { 0x00, 0xFF, 0xFF, "cyan"    },
    { 0xFF, 0xFF, 0xFF, "white"   },
    { 0xFF, 0x80, 0x00, "orange"  },
};

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void die(const char *what) { perror(what); exit(1); }

static void set_edge(struct edge_coef *e, int xa, int ya, int xb, int yb)
{
    e->A = (ya - yb) * 256;
    e->B = (xb - xa) * 256;
    e->C = -(e->A * xa + e->B * ya);
}

static void make_band_quad(struct quad_desc *q, unsigned band, uint8_t color_idx)
{
    int x0 = 0;
    int x1 = (int)VOXEL_RENDER_WIDTH - 1;
    int y0 = (int)(band * VOXEL_BAND_CACHE_HEIGHT);
    int y1 = y0 + (int)VOXEL_BAND_CACHE_HEIGHT - 1;

    memset(q, 0, sizeof(*q));
    q->x_min = (int16_t)x0;
    q->y_min = (int16_t)y0;
    q->x_max = (int16_t)x1;
    q->y_max = (int16_t)y1;

    set_edge(&q->edges[0], x0, y0, x1, y0);
    set_edge(&q->edges[1], x1, y0, x1, y1);
    set_edge(&q->edges[2], x1, y1, x0, y1);
    set_edge(&q->edges[3], x0, y1, x0, y0);

    q->z0 = 0;
    q->dz_dx = 0;
    q->dz_dy = 0;
    q->tex_or_color = color_idx;
    q->flags = 0;
}

int main(int argc, char **argv)
{
    long max_frames = -1;  /* -1 == loop forever */
    if (argc >= 2)
        max_frames = strtol(argv[1], NULL, 0);

    signal(SIGINT, on_sigint);

    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) die("open " DEV_PATH);

    struct voxel_extmem_state ext;
    if (ioctl(fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0) die("ioctl(GET_EXTMEM)");

    printf("extmem before: ctrl=0x%08x front=0x%08x back=0x%08x stride=%u dma=0x%08x\n",
           ext.ctrl, ext.front_base, ext.back_base, ext.stride_bytes, ext.dma_status);

    /* Disable sky-gradient clear: every drawn pixel must come from our quads. */
    ext.ctrl &= ~VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR;
    ext.ctrl |= VOXEL_EXTMEM_CTRL_ENABLE | VOXEL_EXTMEM_CTRL_SCANOUT_EN |
                VOXEL_EXTMEM_CTRL_RGB565;
    if (ioctl(fd, VOXEL_IOC_SET_EXTMEM, &ext) < 0) die("ioctl(SET_EXTMEM)");

    /* Index 0 = bright pink "miss" color so an unrendered pixel screams. */
    {
        struct voxel_palette_entry pe = { 0, 0xFF, 0x00, 0x80 };
        if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &pe) < 0) die("ioctl(SET_PALETTE)");
    }
    /* Indexes 1..8 = per-band colors. */
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        struct voxel_palette_entry pe = {
            .index = (uint8_t)(b + 1),
            .r = BAND_COLORS[b].r,
            .g = BAND_COLORS[b].g,
            .b = BAND_COLORS[b].b,
        };
        if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &pe) < 0) die("ioctl(SET_PALETTE)");
    }

    printf("\nVisually verify on the VGA monitor — bands top to bottom should be:\n");
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        unsigned y0 = b * VOXEL_BAND_CACHE_HEIGHT;
        unsigned y1 = y0 + VOXEL_BAND_CACHE_HEIGHT - 1;
        printf("  band %u (rows %3u..%3u) = %s\n", b, y0, y1, BAND_COLORS[b].name);
    }
    printf("\nAny bright pink stripe = unrendered pixel (clear color).\n");
    printf("Ctrl-C to stop.\n\n");

    long frame = 0;
    while (!g_stop && (max_frames < 0 || frame < max_frames)) {
        if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME) < 0) die("ioctl(CLEAR_FRAME)");

        for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
            struct voxel_band_state bs = {
                .band_index = b,
                .flush_x_min = 0,
                .flush_x_max = VOXEL_RENDER_WIDTH - 1,
                .flush_y_min = 0,
                .flush_y_max = VOXEL_BAND_CACHE_HEIGHT - 1,
            };
            if (ioctl(fd, VOXEL_IOC_BEGIN_BAND, &bs) < 0) die("ioctl(BEGIN_BAND)");
            struct quad_desc q;
            make_band_quad(&q, b, (uint8_t)(b + 1));
            ssize_t n = write(fd, &q, sizeof(q));
            if (n != (ssize_t)sizeof(q)) die("write(quad)");
            if (ioctl(fd, VOXEL_IOC_END_BAND) < 0) die("ioctl(END_BAND)");
        }

        if (ioctl(fd, VOXEL_IOC_FLIP) < 0) die("ioctl(FLIP)");

        if ((frame % 30) == 0) {
            if (ioctl(fd, VOXEL_IOC_GET_EXTMEM, &ext) == 0) {
                int copy_target = (int)((ext.dma_status >> 11) & 1u);
                printf("frame %ld: dma=0x%08x copy_target=%d\n",
                       frame, ext.dma_status, copy_target);
            }
        }

        frame++;
        usleep(33000);  /* ~30 fps */
    }

    printf("\nrendered %ld frames; check VGA output for band correctness.\n", frame);
    close(fd);
    return 0;
}
