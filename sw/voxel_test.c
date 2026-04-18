/*
 * voxel_test.c — minimal user-space smoke test for /dev/voxel_gpu.
 *
 * Exercises the MVP pipeline:
 *   1. Open the device.
 *   2. Print STATUS and FRAME_COUNT.
 *   3. Upload a tiny palette.
 *   4. Run a few frames of: CLEAR -> push one dummy quad -> FLIP.
 *
 * The "quad" is 16 dummy 32-bit words. The driver does not interpret
 * descriptor contents; replace with real geometry once the rasterizer
 * format is finalized.
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
	};
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (ioctl(fd, VOXEL_IOC_SET_PALETTE, &entries[i]))
			die("ioctl(SET_PALETTE)");
	}
}

/*
 * Push a real flat-shaded quad descriptor: a red rectangle (80,60)-(240,180).
 * palette[0] = background (dark), palette[1] = red.
 * The initial RTL milestone uses x/y bounds + color and ignores texturing.
 */
static void push_rect_quad(int fd)
{
	struct quad_desc q = {
		.x_min = 80,  .y_min = 60,
		.x_max = 240, .y_max = 180,
		.edges = {
			{       0,  160*256, -9600*256  },  /* bottom */
			{ -120*256,       0,  28800*256 },  /* right  */
			{       0, -160*256,  28800*256 },  /* top    */
			{  120*256,       0, -9600*256  },  /* left   */
		},
		.z0         = 0x4000,
		.dz_dx      = 0,
		.dz_dy      = 0,
		.tex_or_color = 1,   /* palette index 1 = red */
		.flags      = 0,     /* flat color, z-test off for now */
	};
	ssize_t n = write(fd, &q, sizeof(q));
	if (n < 0)
		die("write(rect_quad)");
	if (n != (ssize_t)sizeof(q))
		fprintf(stderr, "short write: %zd / %zu\n", n, sizeof(q));
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

	printf("single-quad test (red rectangle 80,60 -> 240,180):\n");
	for (frame = 0; frame < 5; frame++) {
		if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME))
			die("ioctl(CLEAR_FRAME)");

		push_rect_quad(fd);

		if (ioctl(fd, VOXEL_IOC_FLIP))
			die("ioctl(FLIP)");

		printf("frame %d done; ", frame);
		print_status(fd);
	}

	close(fd);
	return 0;
}
