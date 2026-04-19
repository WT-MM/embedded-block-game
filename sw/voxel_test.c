/*
 * voxel_test.c — flat-color smoke test for /dev/voxel_gpu.
 *
 * Exercises the current hardware MVP path:
 *   1. Open the device.
 *   2. Print STATUS and FRAME_COUNT.
 *   3. Upload a vivid demo palette.
 *   4. Run a few frames of: CLEAR -> push several real quads -> FLIP.
 *
 * Each frame submits:
 *   * 2 z-tested overlapping rectangles with different depths
 *   * 2 non-overlapping rectangles in different colors
 *   * 2 overlapping rectangles in different colors
 *   * 1 animated sweep quad
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
#define TEST_FRAMES 24
#define FRAME_DELAY_US 80000
#define TEST_QUADS_PER_FRAME 7

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
static struct quad_desc make_rect_quad(int x0, int y0, int x1, int y1,
				       uint8_t color, uint16_t z0, uint8_t flags)
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
	q.dz_dx = 0;
	q.dz_dy = 0;
	q.tex_or_color = color;
	q.flags = flags;

	return q;
}

static void push_test_quads(int fd, int frame)
{
	struct quad_desc quads[TEST_QUADS_PER_FRAME];
	int sweep_x = 20 + ((frame * 10) % 220);

	/* Near magenta should stay visible even though the farther cyan quad is submitted later. */
	quads[0] = make_rect_quad(24, 24, 88, 88, 5, 0x2000, QUAD_FLAG_ZTEST);
	quads[1] = make_rect_quad(40, 40, 104, 104, 6, 0x5000, QUAD_FLAG_ZTEST);
	quads[2] = make_rect_quad(16, 20, 96, 92, 1, 0x4000, 0);
	quads[3] = make_rect_quad(208, 28, 300, 96, 2, 0x4000, 0);
	quads[4] = make_rect_quad(72, 112, 216, 208, 3, 0x4000, 0);
	quads[5] = make_rect_quad(132, 144, 276, 224, 4, 0x4000, 0);
	quads[6] = make_rect_quad(sweep_x, 188, sweep_x + 44, 224, 6, 0x4000, 0);

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

	printf("multi-quad flat-color test:\n");
	printf("  magenta/cyan overlap at top-left uses z-test (magenta is nearer)\n");
	printf("  red + green are non-overlapping\n");
	printf("  blue + yellow overlap to expose submission order\n");
	printf("  cyan sweep moves across the bottom of the screen\n");
	for (frame = 0; frame < TEST_FRAMES; frame++) {
		if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME))
			die("ioctl(CLEAR_FRAME)");

		push_test_quads(fd, frame);

		if (ioctl(fd, VOXEL_IOC_FLIP))
			die("ioctl(FLIP)");

		printf("frame %d done; ", frame);
		print_status(fd);
		usleep(FRAME_DELAY_US);
	}

	close(fd);
	return 0;
}
