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
#define WORDS_PER_QUAD 16

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

static void push_dummy_quad(int fd, uint32_t seed)
{
	uint32_t quad[WORDS_PER_QUAD];
	ssize_t n;
	int i;

	for (i = 0; i < WORDS_PER_QUAD; i++)
		quad[i] = seed + (uint32_t)i;

	n = write(fd, quad, sizeof(quad));
	if (n < 0)
		die("write(quad)");
	if (n != (ssize_t)sizeof(quad))
		fprintf(stderr, "short write: %zd / %zu\n", n, sizeof(quad));
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

	for (frame = 0; frame < 5; frame++) {
		if (ioctl(fd, VOXEL_IOC_CLEAR_FRAME))
			die("ioctl(CLEAR_FRAME)");

		push_dummy_quad(fd, 0xDEAD0000u + (uint32_t)frame);

		if (ioctl(fd, VOXEL_IOC_FLIP))
			die("ioctl(FLIP)");

		printf("frame %d done; ", frame);
		print_status(fd);
	}

	close(fd);
	return 0;
}
