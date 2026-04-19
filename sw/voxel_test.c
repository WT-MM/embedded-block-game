/*
 * voxel_test.c — flat-color smoke test for /dev/voxel_gpu.
 *
 * Exercises the current hardware MVP path:
 *   1. Open the device.
 *   2. Print STATUS and FRAME_COUNT.
 *   3. Upload a vivid demo palette.
 *   4. Run a few frames of: CLEAR -> push several real quads -> FLIP.
 *
 * Each frame submits four focused z-buffer tests:
 *   * top-left: z-tested overlap, submission order swaps by phase
 *   * top-right: z-disabled overlap control, submission order swaps by phase
 *   * bottom-left: equal-depth z-tested overlap, submission order swaps by phase
 *   * bottom-right: z-tested overlap with a non-zero dz_dx depth gradient
 *
 * Build: see Makefile.  Run: ./voxel_test
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "voxel_gpu.h"

#define DEV_PATH "/dev/voxel_gpu"
#define TEST_FRAMES 48
#define PHASE_FRAMES 12
#define FRAME_DELAY_US 80000
#define TEST_QUADS_PER_FRAME 8

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static void print_status(int fd)
{
	struct voxel_status s;
	uint32_t frames;

	if (ioctl(fd, VOXEL_IOC_GET_STATUS, &s))
		die("ioctl(GET_STATUS)");
	if (ioctl(fd, VOXEL_IOC_GET_FRAME_COUNT, &frames))
		die("ioctl(GET_FRAME_COUNT)");

	printf("status: raw=0x%08x fifo=%u busy=%u ffl=%u fem=%u vsy=%u | frames=%u\n",
	       s.raw, s.fifo_count, s.busy, s.fifo_full, s.fifo_empty,
	       s.vsync, frames);
}

static void upload_demo_palette(int fd)
{
	/* A handful of vivid entries; index 0 stays as background. */
	const struct voxel_palette_entry entries[] = {
		{ 0, 0x10, 0x10, 0x18 },   /* dim background */
		{ 1, 0xff, 0x00, 0x00 },   /* red */
		{ 2, 0x00, 0xff, 0x00 },   /* green */
		{ 3, 0x00, 0x80, 0xff },   /* blue */
		{ 4, 0xff, 0xff, 0x00 },   /* yellow */
		{ 5, 0xff, 0x00, 0xff },   /* magenta */
		{ 6, 0x00, 0xff, 0xff },   /* cyan */
		{ 7, 0xff, 0xff, 0xff },   /* white */
		{ 8, 0xff, 0x80, 0x00 },   /* orange */
	};
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &entries[i]))
			die("ioctl(SET_PALETTE)");
	}
}

static void set_edge(struct edge_coef *edge, int x0, int y0, int x1, int y1)
{
	edge->A = (y0 - y1) * 256;
	edge->B = (x1 - x0) * 256;
	edge->C = -(edge->A * x0 + edge->B * y0);
}

/*
 * Build an axis-aligned rectangle descriptor. Vertices are emitted in
 * screen-space clockwise order so the edge tests match the RTL.
 */
static struct quad_desc make_rect_quad_zgrad(int x0, int y0, int x1, int y1,
					     uint8_t color, uint16_t z0,
					     int16_t dz_dx, int16_t dz_dy,
					     uint8_t flags)
{
	struct quad_desc q;

	memset(&q, 0, sizeof(q));
	q.x_min = x0;
	q.y_min = y0;
	q.x_max = x1;
	q.y_max = y1;

	set_edge(&q.edges[0], x0, y0, x1, y0);
	set_edge(&q.edges[1], x1, y0, x1, y1);
	set_edge(&q.edges[2], x1, y1, x0, y1);
	set_edge(&q.edges[3], x0, y1, x0, y0);

	q.z0 = z0;
	q.dz_dx = dz_dx;
	q.dz_dy = dz_dy;
	q.tex_or_color = color;
	q.flags = flags;

	return q;
}

static struct quad_desc make_rect_quad(int x0, int y0, int x1, int y1,
				       uint8_t color, uint16_t z0, uint8_t flags)
{
	return make_rect_quad_zgrad(x0, y0, x1, y1, color, z0, 0, 0, flags);
}

static void push_test_quads(int fd, int frame)
{
	struct quad_desc quads[TEST_QUADS_PER_FRAME];
	int swap_order = ((frame / PHASE_FRAMES) & 1) != 0;
	int q = 0;

	/* Top-left: z-test enabled. Magenta is nearer and must win regardless of order. */
	if (!swap_order) {
		quads[q++] = make_rect_quad(20, 20, 104, 104, 5, 0x2000, QUAD_FLAG_ZTEST);
		quads[q++] = make_rect_quad(44, 44, 128, 128, 6, 0x5000, QUAD_FLAG_ZTEST);
	} else {
		quads[q++] = make_rect_quad(44, 44, 128, 128, 6, 0x5000, QUAD_FLAG_ZTEST);
		quads[q++] = make_rect_quad(20, 20, 104, 104, 5, 0x2000, QUAD_FLAG_ZTEST);
	}

	/* Top-right: z-test disabled control. The overlap winner should flip with order. */
	if (!swap_order) {
		quads[q++] = make_rect_quad(188, 20, 272, 104, 1, 0x4000, 0);
		quads[q++] = make_rect_quad(212, 44, 296, 128, 2, 0x4000, 0);
	} else {
		quads[q++] = make_rect_quad(212, 44, 296, 128, 2, 0x4000, 0);
		quads[q++] = make_rect_quad(188, 20, 272, 104, 1, 0x4000, 0);
	}

	/* Bottom-left: equal z with z-test on. First submitted quad should hold the overlap. */
	if (!swap_order) {
		quads[q++] = make_rect_quad(20, 128, 104, 212, 3, 0x3800, QUAD_FLAG_ZTEST);
		quads[q++] = make_rect_quad(44, 152, 128, 236, 4, 0x3800, QUAD_FLAG_ZTEST);
	} else {
		quads[q++] = make_rect_quad(44, 152, 128, 236, 4, 0x3800, QUAD_FLAG_ZTEST);
		quads[q++] = make_rect_quad(20, 128, 104, 212, 3, 0x3800, QUAD_FLAG_ZTEST);
	}

	/*
	 * Bottom-right: white quad slopes from near on the left to far on the right.
	 * Orange base is constant depth, so the overlap should split with a stable
	 * near/far boundary instead of one quad fully winning.
	 */
	quads[q++] = make_rect_quad(180, 132, 300, 228, 8, 0x3800, QUAD_FLAG_ZTEST);
	quads[q++] = make_rect_quad_zgrad(156, 116, 276, 212, 7, 0x2000, 0x0080, 0,
					  QUAD_FLAG_ZTEST);

	ssize_t n = write(fd, quads, sizeof(quads));
	if (n < 0)
		die("write(test_quads)");
	if (n != (ssize_t)sizeof(quads))
		fprintf(stderr, "short write: %zd / %zu\n", n, sizeof(quads));
}

int main(void)
{
	int fd;
	int frame;

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0)
		die("open " DEV_PATH);

	printf("voxel_test: opened %s\n", DEV_PATH);
	print_status(fd);

	upload_demo_palette(fd);
	printf("uploaded demo palette\n");

	printf("z-buffer diagnostic:\n");
	printf("  top-left    : z-test ON, magenta should always cover cyan in the overlap\n");
	printf("  top-right   : z-test OFF control, red/green overlap should flip every %d frames\n",
	       PHASE_FRAMES);
	printf("  bottom-left : equal-depth z-test, overlap should follow first-submitted color\n");
	printf("  bottom-right: white-on-orange sloped-depth test; left side white, right side orange\n");
	for (frame = 0; frame < TEST_FRAMES; frame++) {
		int swap_order = ((frame / PHASE_FRAMES) & 1) != 0;

		if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME))
			die("ioctl(CLEAR_FRAME)");

		push_test_quads(fd, frame);

		if (ioctl(fd, VOXEL_IOC_FLIP))
			die("ioctl(FLIP)");

		printf("frame %d phase=%d (%s order) done; ",
		       frame, frame / PHASE_FRAMES, swap_order ? "reversed" : "normal");
		print_status(fd);
		usleep(FRAME_DELAY_US);
	}

	close(fd);
	return 0;
}
