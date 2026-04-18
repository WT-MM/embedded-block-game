/*
 * Userspace program that communicates with the vga_ball device driver
 * through ioctls
 *
 * Stephen A. Edwards
 * Columbia University
 */

#include <stdio.h>
#include "vga_ball.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int vga_ball_fd;

/* Read and print the position color */
void print_position() {
  vga_ball_arg_t vla;
  
  if (ioctl(vga_ball_fd, VGA_BALL_READ_POSITION, &vla)) {
      perror("ioctl(VGA_BALL_READ_POSITION) failed");
      return;
  }
  printf("%02x %02x\n",
	 vla.center.x_coord, vla.center.y_coord);
}

/* Set the position color */
void set_position(const vga_ball_pos_t *c)
{
  vga_ball_arg_t vla;
  vla.center = *c;
  if (ioctl(vga_ball_fd, VGA_BALL_WRITE_POSITION, &vla)) {
      perror("ioctl(VGA_BALL_WRITE_POSITION) failed");
      return;
  }
}



int main()
{
  vga_ball_arg_t vla;
  int i;
  static const char filename[] = "/dev/vga_ball";


  printf("VGA ball Userspace program started\n");

  if ( (vga_ball_fd = open(filename, O_RDWR)) == -1) {
    fprintf(stderr, "could not open %s\n", filename);
    return -1;
  }

  printf("initial state: ");
  print_position_color();

  for (i = 0 ; i < 420 ; i++) {
	  struct vga_ball_pos_t vibes = { i, i };
    set_position(vibes);
    print_position();
    usleep(40000);
  }
  
  printf("VGA BALL Userspace program terminating\n");
  return 0;
}
