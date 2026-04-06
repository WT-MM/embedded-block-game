#ifndef _VGA_BALL_H
#define _VGA_BALL_H

#include <linux/ioctl.h>

typedef struct {
  unsigned short x_coord, y_coord;
} vga_ball_pos_t;

typedef struct {
	vga_ball_pos_t center;
} vga_ball_arg_t;

#define VGA_BALL_MAGIC 'q'

/* ioctls and their arguments */
#define VGA_BALL_WRITE_POSITION _IOW(VGA_BALL_MAGIC, 1, vga_ball_arg_t)
#define VGA_BALL_READ_POSITION _IOR(VGA_BALL_MAGIC, 2, vga_ball_arg_t)

#endif
