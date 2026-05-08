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
 *   0x0014  FOG_RANGE   [15:0]=start_dist [31:16]=end_dist       (W)
 *   0x0018  FOG_CTRL    [31:16]=inv_proj_sq [8]=EN [7:0]=pal idx (W)
 *   0x001C  EXTMEM_CTRL  SDRAM display-path control               (R/W)
 *   0x0020  EXTMEM_FRONT_BASE front color buffer byte address     (R/W)
 *   0x0024  EXTMEM_BACK_BASE  second color buffer byte address    (R/W)
 *   0x0028  EXTMEM_STRIDE bytes per scanline in SDRAM             (R/W)
 *   0x002C  EXTMEM_TILE  [15:0]=tile_w [31:16]=tile_h            (R/W)
 *   0x0030  EXTMEM_STAT  SDRAM copy/scanout status                (R)
 *   0x0034  BAND_INDEX   active 64-line render band index          (R/W)
 *   0x0038  BAND_CTRL    [1]=FLUSH [0]=BEGIN band command pulses   (W)
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
#define VOXEL_REG_FOG_RANGE     0x0014
#define VOXEL_REG_FOG_CTRL      0x0018
#define VOXEL_REG_EXTMEM_CTRL   0x001C
#define VOXEL_REG_EXTMEM_FRONT  0x0020
#define VOXEL_REG_EXTMEM_BACK   0x0024
#define VOXEL_REG_EXTMEM_STRIDE 0x0028
#define VOXEL_REG_EXTMEM_TILE   0x002C
#define VOXEL_REG_EXTMEM_STAT   0x0030
#define VOXEL_REG_BAND_INDEX    0x0034
#define VOXEL_REG_BAND_CTRL     0x0038
#define VOXEL_REG_PERF_DRAW_ACT  0x0040
#define VOXEL_REG_PERF_DRAW_IDLE 0x0044
#define VOXEL_REG_PERF_FLUSH_ACT 0x0048
#define VOXEL_REG_PERF_FLUSH_STL 0x004C
#define VOXEL_REG_PERF_INIT      0x0050
#define VOXEL_REG_PERF_LOAD      0x0054
#define VOXEL_REG_PERF_FLUSH_LOAD  0x0058
#define VOXEL_REG_PERF_FLUSH_FIFO  0x005C
#define VOXEL_REG_PERF_FLUSH_DATA  0x0060
#define VOXEL_REG_PERF_FLUSH_DRAIN 0x0064

#define VOXEL_FIFO_BASE         0x1000
#define VOXEL_FIFO_END          0x3000          /* exclusive */
#define VOXEL_FIFO_BYTES        (VOXEL_FIFO_END - VOXEL_FIFO_BASE)
#define VOXEL_FIFO_WORDS        (VOXEL_FIFO_BYTES / 4)   /* 2048 */

#define VOXEL_REG_SPAN          VOXEL_FIFO_END

/* ----- render/display geometry ----- */
/*
 * Target geometry for the SDRAM-backed renderer. Userspace submits the same
 * descriptor stream renderer.c already builds; gpu_transport.c bins that stream
 * into 64-line passes and uses BEGIN_BAND/END_BAND to make the hardware render
 * one resident on-chip band before flushing color to SDRAM.
 */
#define VOXEL_RENDER_WIDTH      640u
#define VOXEL_RENDER_HEIGHT     480u
#define VOXEL_RENDER_STRIDE     (VOXEL_RENDER_WIDTH * 2u) /* RGB565 bytes */
#define VOXEL_BAND_CACHE_HEIGHT 60u
#define VOXEL_BAND_COUNT        ((VOXEL_RENDER_HEIGHT + VOXEL_BAND_CACHE_HEIGHT - 1u) / \
                                 VOXEL_BAND_CACHE_HEIGHT)

/* ----- CONTROL bits ----- */
#define VOXEL_CTRL_EN           (1u << 0)
#define VOXEL_CTRL_FLP          (1u << 1)
#define VOXEL_CTRL_IEN          (1u << 2)
#define VOXEL_CTRL_CLR          (1u << 3)

/* ----- BAND_CTRL bits ----- */
#define VOXEL_BAND_CTRL_BEGIN   (1u << 0)
#define VOXEL_BAND_CTRL_FLUSH   (1u << 1)

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

struct voxel_fog_state {
	__u16 start_dist;       /* Q8.8 radial distance at which fog begins */
	__u16 end_dist;         /* Q8.8 radial distance at which fog becomes solid */
	__u8  color_index;      /* palette index to dither toward */
	__u8  enabled;          /* non-zero enables fog for flagged quads */
	__u16 inv_proj_sq;      /* Q0.16 approximation of 1 / projection_depth^2 */
};

struct voxel_extmem_state {
	__u32 ctrl;             /* VOXEL_EXTMEM_CTRL_* */
	__u32 front_base;       /* byte address of SDRAM frame 0 */
	__u32 back_base;        /* byte address of SDRAM frame 1 */
	__u32 stride_bytes;     /* bytes per scanline */
	__u32 tile_cfg;         /* reserved for later tile-cache work */
	__u32 dma_status;       /* read-only SDRAM copy/scanout status */
};

struct voxel_band_state {
	__u32 band_index;       /* 0..VOXEL_BAND_COUNT-1 */
};

struct voxel_perf_counters {
	__u32 draw_active;      /* cycles draw committing pixel */
	__u32 draw_idle;        /* cycles draw stalled (descriptor starved) */
	__u32 flush_active;     /* cycles bg flush pushing word to SDRAM */
	__u32 flush_stall;      /* cycles bg flush stalled (SDRAM/scanout) */
	__u32 init;             /* cycles in cache init */
	__u32 load;             /* cycles in cache load/drain */
};

struct voxel_perf_counters_v2 {
	struct voxel_perf_counters base;
	__u32 flush_wait_load;   /* cycles waiting to launch WR stream */
	__u32 flush_wait_fifo;   /* cycles blocked by write FIFO/headroom */
	__u32 flush_wait_data;   /* cycles waiting on cache/sky word */
	__u32 flush_wait_drain;  /* cycles final SDRAM/FIFO drain */
};

/* ----- ioctl numbers ----- */
#define VOXEL_IOC_MAGIC 'v'

#define VOXEL_IOC_CLEAR_FRAME      _IO(VOXEL_IOC_MAGIC, 1)
#define VOXEL_IOC_FLIP             _IO(VOXEL_IOC_MAGIC, 2)
#define VOXEL_IOC_SET_PALETTE      _IOW(VOXEL_IOC_MAGIC, 3, struct voxel_palette_entry)
#define VOXEL_IOC_GET_STATUS       _IOR(VOXEL_IOC_MAGIC, 4, struct voxel_status)
#define VOXEL_IOC_GET_FRAME_COUNT  _IOR(VOXEL_IOC_MAGIC, 5, __u32)
#define VOXEL_IOC_SET_FOG          _IOW(VOXEL_IOC_MAGIC, 6, struct voxel_fog_state)
#define VOXEL_IOC_SET_EXTMEM       _IOW(VOXEL_IOC_MAGIC, 7, struct voxel_extmem_state)
#define VOXEL_IOC_GET_EXTMEM       _IOR(VOXEL_IOC_MAGIC, 8, struct voxel_extmem_state)
#define VOXEL_IOC_BEGIN_BAND       _IOW(VOXEL_IOC_MAGIC, 9, struct voxel_band_state)
#define VOXEL_IOC_END_BAND         _IO(VOXEL_IOC_MAGIC, 10)
#define VOXEL_IOC_FLIP_ASYNC       _IO(VOXEL_IOC_MAGIC, 11)
#define VOXEL_IOC_WAIT_FLIP        _IO(VOXEL_IOC_MAGIC, 12)
#define VOXEL_IOC_GET_PERF         _IOR(VOXEL_IOC_MAGIC, 13, struct voxel_perf_counters)
#define VOXEL_IOC_GET_PERF2        _IOR(VOXEL_IOC_MAGIC, 14, struct voxel_perf_counters_v2)

#define VOXEL_IOC_MAXNR            14

#define VOXEL_EXTMEM_CTRL_ENABLE        (1u << 0)
#define VOXEL_EXTMEM_CTRL_SCANOUT_EN    (1u << 1)
#define VOXEL_EXTMEM_CTRL_BACKBUF_EN    (1u << 2)
#define VOXEL_EXTMEM_CTRL_RGB565        (1u << 3)
#define VOXEL_EXTMEM_CTRL_TILE_CACHE_EN (1u << 4)
#define VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR (1u << 5)

/* ----- quad descriptor layout (§5.2) ----- */

#define QUAD_FLAG_TEX        (1u << 0)   /* textured: second 64-byte UV block follows */
#define QUAD_FLAG_ZTEST      (1u << 1)   /* enable z-test and z-write */
#define QUAD_FLAG_ALPHA_KEY  (1u << 2)   /* skip texels whose sampled palette index is 0 */
#define QUAD_FLAG_FOG        (1u << 3)   /* apply global depth fog to this quad */
#define QUAD_LIGHT_SHIFT     4           /* bits [5:4] select 64-entry palette light bank */
#define QUAD_LIGHT_MASK      (3u << QUAD_LIGHT_SHIFT)
#define QUAD_LIGHT_LEVEL(n)  ((((unsigned)(n)) & 3u) << QUAD_LIGHT_SHIFT)
#define QUAD_ALPHA_SHIFT     6           /* bits [7:6] select source alpha */
#define QUAD_ALPHA_MASK      (3u << QUAD_ALPHA_SHIFT)
#define QUAD_ALPHA_LEVEL(n)  ((((unsigned)(n)) & 3u) << QUAD_ALPHA_SHIFT)
#define QUAD_ALPHA_OPAQUE    QUAD_ALPHA_LEVEL(0)
#define QUAD_ALPHA_75        QUAD_ALPHA_LEVEL(1)
#define QUAD_ALPHA_50        QUAD_ALPHA_LEVEL(2)
#define QUAD_ALPHA_25        QUAD_ALPHA_LEVEL(3)

/* Textured descriptors use tex_or_color[6:0] as the tile id. Bit 7 asks
 * the texture unit to wrap U/V every 16 texels, including slightly negative
 * fixed-point coordinates from perspective interpolation. This lets software
 * merge same-tile block faces without stretching their textures. */
#define QUAD_TEX_REPEAT_UV   (1u << 7)

struct edge_coef {
	__s32 A, B, C;   /* signed Q24.8 */
};

/* First 64-byte block — always present */
struct quad_desc {
	__s16 x_min, y_min, x_max, y_max;   /* screen-space bounding box */
	struct edge_coef edges[4];           /* 4 × 12 = 48 B */
	__u16 z0;                            /* Q1.15 unsigned, depth at (x_min, y_min) */
	__s16 dz_dx, dz_dy;                  /* Q1.15 signed depth gradients */
	__u8  tex_or_color;                  /* TEX=1: tile id (0-127); TEX=0: palette idx */
	__u8  flags;                         /* QUAD_FLAG_* */
} __attribute__((packed));

/*
 * Second 64-byte block — appended only when QUAD_FLAG_TEX is set.
 *
 * Perspective-correct texturing: interpolate (u/w), (v/w), and (1/w) linearly
 * in screen space, then divide per-pixel in hardware to recover true (u, v).
 * Each plane is stored as (value at (x_min, y_min), dx gradient, dy gradient)
 * in Q16.16. 1/w is positive for any pixel in front of the near plane.
 */
struct quad_desc_uv {
	__s32 u_over_w_0;         /* Q16.16, u/w at (x_min, y_min) */
	__s32 u_over_w_dx;        /* Q16.16, u/w gradient along x */
	__s32 u_over_w_dy;        /* Q16.16, u/w gradient along y */
	__s32 v_over_w_0;         /* Q16.16, v/w at (x_min, y_min) */
	__s32 v_over_w_dx;        /* Q16.16, v/w gradient along x */
	__s32 v_over_w_dy;        /* Q16.16, v/w gradient along y */
	__s32 one_over_w_0;       /* Q16.16, 1/w at (x_min, y_min) */
	__s32 one_over_w_dx;      /* Q16.16, 1/w gradient along x */
	__s32 one_over_w_dy;      /* Q16.16, 1/w gradient along y */
	__u8  reserved[28];       /* write 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct edge_coef) == 12, "edge_coef must be 12 bytes");
_Static_assert(sizeof(struct quad_desc) == 64, "quad_desc must be 64 bytes");
_Static_assert(sizeof(struct quad_desc_uv) == 64, "quad_desc_uv must be 64 bytes");
_Static_assert(sizeof(struct voxel_fog_state) == 8, "voxel_fog_state must be 8 bytes");
_Static_assert(sizeof(struct voxel_extmem_state) == 24, "voxel_extmem_state must be 24 bytes");
_Static_assert(sizeof(struct voxel_band_state) == 4, "voxel_band_state must be 4 bytes");

#endif /* _VOXEL_GPU_H */
