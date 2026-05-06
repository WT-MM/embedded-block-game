/*
 * fpga_band_offset_test.c — verify each render band lands at its expected
 * SDRAM offset and not on top of another band.
 *
 * Each of the 8 bands gets a unique flat-color quad spanning that band's
 * full 640x64 region. After FLIP we mmap the SDRAM frame buffer through
 * /dev/mem and check every band region holds exactly one expected color.
 *
 * If the recent vertical-duplication bug is caused by band-offset aliasing
 * (e.g. band 4 colliding with band 0, or stride miscount), this test will
 * report the FAILing band(s) and which other band's color appears in their
 * place.
 *
 * Run as root (needs /dev/mem):
 *     sudo ./tests/fpga_band_offset_test
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "voxel_gpu.h"

#define DEV_PATH    "/dev/voxel_gpu"
#define MEM_PATH    "/dev/mem"
#define BRIDGE_BASE 0xC0000000ull
#define FRAME_BYTES (VOXEL_RENDER_WIDTH * VOXEL_RENDER_HEIGHT * 2u)

/* From the dma_status assembly in voxel_gpu.sv: bit 11 = copy_target_sel. */
#define EXTMEM_STAT_COPY_TARGET_SHIFT 11u

struct band_color { uint8_t r, g, b; };

static const struct band_color BAND_COLORS[VOXEL_BAND_COUNT] = {
    { 0xFF, 0x00, 0x00 },  /* 0 red     */
    { 0x00, 0xFF, 0x00 },  /* 1 green   */
    { 0x00, 0x80, 0xFF },  /* 2 blue    */
    { 0xFF, 0xFF, 0x00 },  /* 3 yellow  */
    { 0xFF, 0x00, 0xFF },  /* 4 magenta */
    { 0x00, 0xFF, 0xFF },  /* 5 cyan    */
    { 0xFF, 0xFF, 0xFF },  /* 6 white   */
    { 0xFF, 0x80, 0x00 },  /* 7 orange  */
};

static void die(const char *what) { perror(what); exit(1); }

static uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int identify_band(uint16_t pixel)
{
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        uint16_t c = rgb_to_565(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b);
        if (pixel == c)
            return (int)b;
    }
    return -1;
}

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

struct buf_view {
    const char *name;
    uint64_t phys;
    uint64_t map_base;
    uint64_t map_off;
    uint64_t map_len;
    void *map;
    volatile uint16_t *pixels;
};

static void map_buffer(int mem_fd, long page, struct buf_view *v)
{
    v->map_base = v->phys & ~((uint64_t)page - 1u);
    v->map_off  = v->phys - v->map_base;
    v->map_len  = v->map_off + FRAME_BYTES;
    v->map = mmap(NULL, (size_t)v->map_len, PROT_READ | PROT_WRITE,
                  MAP_SHARED, mem_fd, (off_t)v->map_base);
    if (v->map == MAP_FAILED) die("mmap");
    v->pixels = (volatile uint16_t *)((uint8_t *)v->map + v->map_off);
}

static int score_buffer(const struct buf_view *v)
{
    int hits = 0;
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        unsigned y = b * VOXEL_BAND_CACHE_HEIGHT + VOXEL_BAND_CACHE_HEIGHT / 2;
        unsigned x = VOXEL_RENDER_WIDTH / 2;
        uint16_t want = rgb_to_565(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b);
        if (v->pixels[y * VOXEL_RENDER_WIDTH + x] == want)
            hits++;
    }
    return hits;
}

int main(void)
{
    int hw_fd = open(DEV_PATH, O_RDWR);
    if (hw_fd < 0) die("open " DEV_PATH);

    struct voxel_extmem_state ext;
    if (ioctl(hw_fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0) die("ioctl(GET_EXTMEM)");

    printf("extmem before: ctrl=0x%08x front=0x%08x back=0x%08x stride=%u dma=0x%08x\n",
           ext.ctrl, ext.front_base, ext.back_base, ext.stride_bytes, ext.dma_status);

    if (ext.stride_bytes != VOXEL_RENDER_STRIDE) {
        fprintf(stderr, "extmem stride %u != %u (test assumes %u)\n",
                ext.stride_bytes, VOXEL_RENDER_STRIDE, VOXEL_RENDER_STRIDE);
        return 1;
    }

    /* Disable sky-gradient clear so unrendered pixels stay deterministic. */
    ext.ctrl &= ~VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR;
    ext.ctrl |= VOXEL_EXTMEM_CTRL_ENABLE | VOXEL_EXTMEM_CTRL_SCANOUT_EN |
                VOXEL_EXTMEM_CTRL_RGB565;
    if (ioctl(hw_fd, VOXEL_IOC_SET_EXTMEM, &ext) < 0) die("ioctl(SET_EXTMEM)");

    /* Palette index b+1 = band b's color; index 0 = dim "miss" color. */
    {
        struct voxel_palette_entry pe = { 0, 0x10, 0x10, 0x10 };
        if (ioctl(hw_fd, VOXEL_IOC_SET_PALETTE, &pe) < 0) die("ioctl(SET_PALETTE)");
    }
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        struct voxel_palette_entry pe = {
            .index = (uint8_t)(b + 1),
            .r = BAND_COLORS[b].r,
            .g = BAND_COLORS[b].g,
            .b = BAND_COLORS[b].b,
        };
        if (ioctl(hw_fd, VOXEL_IOC_SET_PALETTE, &pe) < 0) die("ioctl(SET_PALETTE)");
    }

    /* Render two consecutive frames so both buffers contain the same pattern;
     * eliminates "we read the wrong buffer" as a failure mode. */
    for (int frame = 0; frame < 2; frame++) {
        if (ioctl(hw_fd, VOXEL_IOC_CLEAR_FRAME) < 0) die("ioctl(CLEAR_FRAME)");
        for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
            struct voxel_band_state bs = { .band_index = b };
            if (ioctl(hw_fd, VOXEL_IOC_BEGIN_BAND, &bs) < 0) die("ioctl(BEGIN_BAND)");
            struct quad_desc q;
            make_band_quad(&q, b, (uint8_t)(b + 1));
            ssize_t n = write(hw_fd, &q, sizeof(q));
            if (n != (ssize_t)sizeof(q)) die("write(quad)");
            if (ioctl(hw_fd, VOXEL_IOC_END_BAND) < 0) die("ioctl(END_BAND)");
        }
        if (ioctl(hw_fd, VOXEL_IOC_FLIP) < 0) die("ioctl(FLIP)");
    }

    if (ioctl(hw_fd, VOXEL_IOC_GET_EXTMEM, &ext) < 0) die("ioctl(GET_EXTMEM after)");
    int copy_target = (int)((ext.dma_status >> EXTMEM_STAT_COPY_TARGET_SHIFT) & 1u);
    printf("extmem after:  dma=0x%08x copy_target=%d\n", ext.dma_status, copy_target);

    int mem_fd = open(MEM_PATH, O_RDWR | O_SYNC);
    if (mem_fd < 0) die("open " MEM_PATH);
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) die("sysconf");

    struct buf_view views[2] = {
        { "front", BRIDGE_BASE + ext.front_base, 0,0,0, NULL, NULL },
        { "back",  BRIDGE_BASE + ext.back_base,  0,0,0, NULL, NULL },
    };
    map_buffer(mem_fd, page, &views[0]);
    map_buffer(mem_fd, page, &views[1]);

    int s0 = score_buffer(&views[0]);
    int s1 = score_buffer(&views[1]);
    int rendered = (s0 >= s1) ? 0 : 1;
    printf("buffer scores: front=%d/%u back=%d/%u — using '%s' (phys 0x%" PRIx64 ")\n",
           s0, VOXEL_BAND_COUNT, s1, VOXEL_BAND_COUNT,
           views[rendered].name, views[rendered].phys);

    volatile uint16_t *pix = views[rendered].pixels;
    int sample_x[5] = { 0, 159, 319, 479, 639 };
    int band_pass = 0, band_fail = 0;

    printf("\n=== Per-band readback ===\n");
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        uint16_t want = rgb_to_565(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b);
        unsigned y_top = b * VOXEL_BAND_CACHE_HEIGHT;
        unsigned y_mid = y_top + VOXEL_BAND_CACHE_HEIGHT / 2;
        unsigned y_bot = y_top + VOXEL_BAND_CACHE_HEIGHT - 1;
        unsigned ys[3] = { y_top, y_mid, y_bot };

        int ok = 1;
        uint16_t bad_pixel = 0;
        unsigned bad_x = 0, bad_y = 0;
        for (int yi = 0; yi < 3 && ok; yi++) {
            for (int xi = 0; xi < 5; xi++) {
                unsigned x = (unsigned)sample_x[xi];
                uint16_t got = pix[ys[yi] * VOXEL_RENDER_WIDTH + x];
                if (got != want) {
                    ok = 0;
                    bad_pixel = got;
                    bad_x = x;
                    bad_y = ys[yi];
                    break;
                }
            }
        }

        if (ok) {
            printf("band %u rows %3u..%3u expected=0x%04x: PASS\n",
                   b, y_top, y_bot, want);
            band_pass++;
        } else {
            int aliased = identify_band(bad_pixel);
            printf("band %u rows %3u..%3u expected=0x%04x: FAIL  "
                   "at (x=%u,y=%u) got=0x%04x",
                   b, y_top, y_bot, want, bad_x, bad_y, bad_pixel);
            if (aliased >= 0)
                printf(" (matches band %d)\n", aliased);
            else
                printf(" (no band match — likely sky/clear/garbage)\n");
            band_fail++;
        }
    }

    printf("\n=== Raw row-start samples (col 0..3) ===\n");
    for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
        unsigned y = b * VOXEL_BAND_CACHE_HEIGHT;
        printf("row %3u (band %u start): ", y, b);
        for (int x = 0; x < 4; x++)
            printf("%04x ", pix[y * VOXEL_RENDER_WIDTH + x]);
        printf("\n");
    }

    printf("\n=== Mid-row samples in unrendered '%s' buffer ===\n",
           views[rendered ^ 1].name);
    {
        volatile uint16_t *opix = views[rendered ^ 1].pixels;
        for (unsigned b = 0; b < VOXEL_BAND_COUNT; b++) {
            unsigned y = b * VOXEL_BAND_CACHE_HEIGHT + VOXEL_BAND_CACHE_HEIGHT / 2;
            printf("row %3u: ", y);
            for (int x = 0; x < 4; x++)
                printf("%04x ", opix[y * VOXEL_RENDER_WIDTH + x]);
            printf("\n");
        }
    }

    printf("\nsummary: %d/%u bands PASS, %d FAIL\n",
           band_pass, VOXEL_BAND_COUNT, band_fail);

    munmap(views[0].map, (size_t)views[0].map_len);
    munmap(views[1].map, (size_t)views[1].map_len);
    close(mem_fd);
    close(hw_fd);
    return band_fail ? 1 : 0;
}
