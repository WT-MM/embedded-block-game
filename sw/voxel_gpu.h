/*
 * voxel_gpu.h — shared user/kernel header for the voxel GPU device driver.
 *
 * This header documents the FPGA peripheral's register layout and defines
 * the ioctl ABI exposed through /dev/voxel_gpu.
 *
 * Register map (Avalon-MM slave, byte offsets, 32-bit words):
 *   0x0000  CONTROL     [3]=CLR [2]=IEN [1]=FLP [0]=EN          (R/W)
 *   0x0004  STATUS      [19:4]=FIFO_COUNT [3]=VSY [2]=FEM
 *                       [1]=FFL [0]=BSY                          (R)
 *   0x0008  FRAME_COUNT 32-bit free-running counter              (R)
 *   0x000C  PALETTE_ADDR  [7:0] = palette index                  (W)
 *   0x0010  PALETTE_DATA  [23:16]=R [15:8]=G [7:0]=B             (W)
 *   0x1000..0x2FFF  FIFO_WINDOW (8 KB / 2048 words)              (W)
 *
 * The driver itself is intentionally dumb: it streams bytes from
 * userspace straight into FIFO_WINDOW and provides ioctls for the
 * handful of control knobs the hardware exposes.
 */

#ifndef _VOXEL_GPU_H
#define _VOXEL_GPU_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef int16_t  __s16;
typedef int32_t  __s32;
#endif

/* ----- register layout (byte offsets) ----- */
#define VOXEL_REG_CONTROL       0x0000
#define VOXEL_REG_STATUS        0x0004
#define VOXEL_REG_FRAME_COUNT   0x0008
#define VOXEL_REG_PALETTE_ADDR  0x000C
#define VOXEL_REG_PALETTE_DATA  0x0010

#define VOXEL_FIFO_BASE         0x1000
#define VOXEL_FIFO_END          0x3000          /* exclusive */
#define VOXEL_FIFO_BYTES        (VOXEL_FIFO_END - VOXEL_FIFO_BASE)
#define VOXEL_FIFO_WORDS        (VOXEL_FIFO_BYTES / 4)   /* 2048 */

#define VOXEL_REG_SPAN          VOXEL_FIFO_END

/* ----- CONTROL bits ----- */
#define VOXEL_CTRL_EN           (1u << 0)
#define VOXEL_CTRL_FLP          (1u << 1)
#define VOXEL_CTRL_IEN          (1u << 2)
#define VOXEL_CTRL_CLR          (1u << 3)

/* ----- STATUS bits ----- */
#define VOXEL_STAT_BSY          (1u << 0)       /* rasterizer busy */
#define VOXEL_STAT_FFL          (1u << 1)       /* FIFO full */
#define VOXEL_STAT_FEM          (1u << 2)       /* FIFO empty */
#define VOXEL_STAT_VSY          (1u << 3)       /* vsync pulse latched */
#define VOXEL_STAT_FIFO_SHIFT   4
#define VOXEL_STAT_FIFO_MASK    0xFFFFu

/* ----- ioctl payloads ----- */
struct voxel_palette_entry {
	__u8 index;
	__u8 r;
	__u8 g;
	__u8 b;
};

struct voxel_status {
	__u32 raw;              /* full STATUS register */
	__u32 fifo_count;       /* convenience: words currently in FIFO */
	__u8  busy;
	__u8  fifo_full;
	__u8  fifo_empty;
	__u8  vsync;
};

/* ----- ioctl numbers ----- */
#define VOXEL_IOC_MAGIC 'v'

#define VOXEL_IOC_CLEAR_FRAME      _IO(VOXEL_IOC_MAGIC, 1)
#define VOXEL_IOC_FLIP             _IO(VOXEL_IOC_MAGIC, 2)
#define VOXEL_IOC_SET_PALETTE      _IOW(VOXEL_IOC_MAGIC, 3, struct voxel_palette_entry)
#define VOXEL_IOC_GET_STATUS       _IOR(VOXEL_IOC_MAGIC, 4, struct voxel_status)
#define VOXEL_IOC_GET_FRAME_COUNT  _IOR(VOXEL_IOC_MAGIC, 5, __u32)

#define VOXEL_IOC_MAXNR            5

/* ----- quad descriptor layout (§5.2) ----- */

#define QUAD_FLAG_TEX    (1u << 0)   /* textured: second 64-byte UV block follows */
#define QUAD_FLAG_ZTEST  (1u << 1)   /* enable z-test and z-write */

struct edge_coef {
	__s32 A, B, C;   /* signed Q24.8 */
};

/* First 64-byte block — always present */
struct quad_desc {
	__s16 x_min, y_min, x_max, y_max;   /* screen-space bounding box */
	struct edge_coef edges[4];           /* 4 × 12 = 48 B */
	__u16 z0;                            /* Q1.15 unsigned, depth at (x_min, y_min) */
	__s16 dz_dx, dz_dy;                  /* Q1.15 signed depth gradients */
	__u8  tex_or_color;                  /* TEX=1: tile id (0-63); TEX=0: palette idx */
	__u8  flags;                         /* QUAD_FLAG_* */
} __attribute__((packed));

/* Second 64-byte block — appended only when QUAD_FLAG_TEX is set */
struct quad_desc_uv {
	__s32 u0, v0;          /* Q16.16, UV at (x_min, y_min) */
	__s32 du_dx, dv_dx;    /* Q16.16, UV gradients along x */
	__s32 du_dy, dv_dy;    /* Q16.16, UV gradients along y */
	__u8  reserved[40];    /* write 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct edge_coef) == 12, "edge_coef must be 12 bytes");
_Static_assert(sizeof(struct quad_desc) == 64, "quad_desc must be 64 bytes");
_Static_assert(sizeof(struct quad_desc_uv) == 64, "quad_desc_uv must be 64 bytes");

#endif /* _VOXEL_GPU_H */
