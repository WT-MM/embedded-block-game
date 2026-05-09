/*
 * voxel_gpu.c — MVP device driver for the FPGA voxel GPU peripheral.
 *
 * Responsibilities (per the MVP spec):
 *   (A) Map the FPGA registers exposed through the lightweight HPS bridge.
 *   (B) Expose /dev/voxel_gpu via the misc subsystem.
 *   (C) Stream user-space command bytes into the on-chip FIFO via write().
 *   (D) Provide the control ioctls: CLEAR_FRAME, FLIP, SET_PALETTE,
 *       GET_STATUS, GET_FRAME_COUNT, SET_FOG, SET_EXTMEM, GET_EXTMEM,
 *       BEGIN_BAND, END_BAND.
 *   (E) Use polling on STATUS for synchronization (no interrupts).
 *
 * The driver is intentionally thin: it does not parse, validate, or
 * schedule descriptors. It is a pass-through pipe between user space
 * and the FIFO.
 *
 * Bind via the device tree compatible string "csee4840,voxel_gpu-1.0".
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <linux/version.h>

#include "voxel_gpu.h"

#define DRIVER_NAME "voxel_gpu"

/*
 * Bounce buffer for write(): we copy from user in chunks, then push the
 * chunk into the FIFO in bursts paced by STATUS.FIFO_COUNT. Sized larger
 * than the 4 KB hardware FIFO so one copy_from_user() feeds multiple
 * burst rounds; the inner loop already back-pressures via
 * voxel_fifo_wait_space, so a bigger bounce just amortizes the
 * outer-loop syscall/copy overhead on heavy bands.
 */
#define VOXEL_BOUNCE_WORDS  2048                      /* 8 KB per chunk */
#define VOXEL_BOUNCE_BYTES  (VOXEL_BOUNCE_WORDS * 4)

/*
 * Polling timeouts. These are very generous — at 60 Hz a vsync arrives
 * every ~16 ms; the rasterizer should never take a full second on any
 * well-formed frame, so a 250 ms timeout is essentially "the hardware
 * is wedged".
 */
#define VOXEL_POLL_TIMEOUT_MS   250
#define VOXEL_POLL_DELAY_US     1
#define VOXEL_FIFO_MIN_BURST_WORDS 64

static bool voxel_diag_upload;
module_param_named(diag_upload, voxel_diag_upload, bool, 0644);
MODULE_PARM_DESC(diag_upload,
		 "Print descriptor upload timing split: FIFO-space wait vs MMIO writes");

struct voxel_gpu_dev {
	struct resource res;
	void __iomem   *base;
	struct mutex    lock;       /* serializes register access */
	u32            *bounce;     /* reusable write() staging buffer */
};

static struct voxel_gpu_dev voxdev;

struct voxel_upload_diag {
	u64 bytes;
	u64 calls;
	u64 bursts;
	u64 wait_ns;
	u64 mmio_ns;
};

static struct voxel_upload_diag upload_diag_accum;
static unsigned long upload_diag_next_report;

/* ---------- low-level register helpers ---------- */

static inline u32 voxel_rd(u32 off)
{
	return ioread32(voxdev.base + off);
}

static inline void voxel_wr(u32 off, u32 val)
{
	iowrite32(val, voxdev.base + off);
}

static inline u32 voxel_status(void)
{
	return voxel_rd(VOXEL_REG_STATUS);
}

static inline u32 voxel_fifo_count(void)
{
	return (voxel_status() >> VOXEL_STAT_FIFO_SHIFT) &
	       VOXEL_STAT_FIFO_MASK;
}

/*
 * Wait until (status & mask) == expect, polling every VOXEL_POLL_DELAY_US.
 * Returns 0 on success, -ETIMEDOUT if the condition never holds.
 */
static int voxel_poll_status(u32 mask, u32 expect, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (time_before(jiffies, deadline)) {
		if ((voxel_status() & mask) == expect)
			return 0;
		udelay(VOXEL_POLL_DELAY_US);
		cond_resched();
	}
	return ((voxel_status() & mask) == expect) ? 0 : -ETIMEDOUT;
}

/* ---------- FIFO streaming ---------- */

static int voxel_fifo_wait_space(size_t *space_words, size_t min_req)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(VOXEL_POLL_TIMEOUT_MS);
	unsigned int short_waits = 0;

	for (;;) {
		u32 used = voxel_fifo_count();
		u32 space = used < VOXEL_FIFO_WORDS ? VOXEL_FIFO_WORDS - used : 0;

		if (space >= min_req) {
			*space_words = space;
			return 0;
		}
		if (space > 0 && ++short_waits >= 8) {
			*space_words = space;
			return 0;
		}
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(2, 5);
	}
}

static void voxel_diag_record_upload(size_t bytes, u64 wait_ns, u64 mmio_ns,
				     u64 bursts)
{
	u64 total_ns;
	u64 total_kib_s;
	u64 mmio_kib_s;
	u64 wait_pct;
	u64 mmio_pct;

	if (!voxel_diag_upload)
		return;

	upload_diag_accum.bytes += bytes;
	upload_diag_accum.calls++;
	upload_diag_accum.bursts += bursts;
	upload_diag_accum.wait_ns += wait_ns;
	upload_diag_accum.mmio_ns += mmio_ns;

	if (!upload_diag_next_report ||
	    time_after_eq(jiffies, upload_diag_next_report)) {
		total_ns = upload_diag_accum.wait_ns + upload_diag_accum.mmio_ns;
		total_kib_s = total_ns ?
			div64_u64(upload_diag_accum.bytes * 1000000000ULL,
				  total_ns * 1024ULL) : 0;
		mmio_kib_s = upload_diag_accum.mmio_ns ?
			div64_u64(upload_diag_accum.bytes * 1000000000ULL,
				  upload_diag_accum.mmio_ns * 1024ULL) : 0;
		wait_pct = total_ns ?
			div64_u64(upload_diag_accum.wait_ns * 100ULL,
				  total_ns) : 0;
		mmio_pct = total_ns ?
			div64_u64(upload_diag_accum.mmio_ns * 100ULL,
				  total_ns) : 0;

		pr_info(DRIVER_NAME
			": upload diag: calls=%llu bytes=%llu bursts=%llu "
			"wait=%lluus(%llu%%) mmio=%lluus(%llu%%) "
			"rate_total=%lluKiB/s rate_mmio=%lluKiB/s\n",
			upload_diag_accum.calls,
			upload_diag_accum.bytes,
			upload_diag_accum.bursts,
			div64_u64(upload_diag_accum.wait_ns, 1000),
			wait_pct,
			div64_u64(upload_diag_accum.mmio_ns, 1000),
			mmio_pct,
			total_kib_s,
			mmio_kib_s);

		memset(&upload_diag_accum, 0, sizeof(upload_diag_accum));
		upload_diag_next_report = jiffies + msecs_to_jiffies(1000);
	}
}

/* ---------- file ops ---------- */

static int voxel_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &voxdev;
	return 0;
}

static int voxel_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * write(): treat the byte stream as a packed sequence of 32-bit FIFO
 * words. The driver does not interpret descriptor boundaries; user
 * space is responsible for handing us properly-aligned, multiple-of-4
 * payloads (typically whole quads = 16 words = 64 bytes).
 */
static ssize_t voxel_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	u32 *bounce;
	size_t total = 0;
	int ret = 0;

	if (count == 0)
		return 0;
	if (count & 0x3)
		return -EINVAL;        /* must be 32-bit aligned */

	bounce = voxdev.bounce;
	if (!bounce)
		return -ENODEV;

	if (mutex_lock_interruptible(&voxdev.lock)) {
		return -ERESTARTSYS;
	}

	while (total < count) {
		size_t chunk = min_t(size_t, count - total, VOXEL_BOUNCE_BYTES);
		size_t words;
		size_t written_words = 0;
		u64 write_wait_ns = 0;
		u64 write_mmio_ns = 0;
		u64 write_bursts = 0;

		if (copy_from_user(bounce, buf + total, chunk)) {
			ret = -EFAULT;
			break;
		}

		words = chunk / 4;
		while (written_words < words) {
			size_t space_words;
			size_t burst_words;
			size_t rem_words = words - written_words;
			size_t min_req = min_t(size_t, VOXEL_FIFO_MIN_BURST_WORDS,
					       rem_words);
			u64 t0;

			t0 = ktime_get_ns();
			ret = voxel_fifo_wait_space(&space_words, min_req);
			write_wait_ns += ktime_get_ns() - t0;
			if (ret)
				goto out;

			burst_words = min(space_words, rem_words);
			t0 = ktime_get_ns();
			iowrite32_rep(voxdev.base + VOXEL_FIFO_BASE,
				      &bounce[written_words],
				      burst_words);
			write_mmio_ns += ktime_get_ns() - t0;
			write_bursts++;

			written_words += burst_words;
		}
		total += chunk;
		voxel_diag_record_upload(chunk, write_wait_ns, write_mmio_ns,
					 write_bursts);
	}

out:
	mutex_unlock(&voxdev.lock);

	if (total > 0)
		return total;
	return ret;
}

/* ---------- ioctl handlers ---------- */

static long voxel_ioc_clear(void)
{
	u32 ctrl;
	int ret;

	mutex_lock(&voxdev.lock);

	/* Pulse CLR with EN held high; hardware self-clears CLR. */
	ctrl = voxel_rd(VOXEL_REG_CONTROL) | VOXEL_CTRL_EN | VOXEL_CTRL_CLR;
	voxel_wr(VOXEL_REG_CONTROL, ctrl);

	ret = voxel_poll_status(VOXEL_STAT_BSY, 0, VOXEL_POLL_TIMEOUT_MS);

	mutex_unlock(&voxdev.lock);
	return ret;
}

static long voxel_ioc_flip_common(bool wait_for_vsync)
{
	u32 ctrl;
	int ret;

	mutex_lock(&voxdev.lock);

	/*
	 * Drain the FIFO before requesting a flip — the rasterizer must
	 * have consumed all pending quads before we swap buffers.
	 */
	ret = voxel_poll_status(VOXEL_STAT_FEM, VOXEL_STAT_FEM,
				VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;

	ret = voxel_poll_status(VOXEL_STAT_BSY, 0, VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;

	ctrl = voxel_rd(VOXEL_REG_CONTROL) | VOXEL_CTRL_EN | VOXEL_CTRL_FLP;
	voxel_wr(VOXEL_REG_CONTROL, ctrl);

	if (!wait_for_vsync)
		goto out;

	/* Block until the next vsync pulse latches. */
	ret = voxel_poll_status(VOXEL_STAT_VSY, VOXEL_STAT_VSY,
				VOXEL_POLL_TIMEOUT_MS);

out:
	mutex_unlock(&voxdev.lock);
	return ret;
}

static long voxel_ioc_flip(void)
{
	return voxel_ioc_flip_common(true);
}

static long voxel_ioc_flip_async(void)
{
	return voxel_ioc_flip_common(false);
}

static long voxel_ioc_wait_flip(void)
{
	int ret;

	mutex_lock(&voxdev.lock);
	ret = voxel_poll_status(VOXEL_STAT_VSY, VOXEL_STAT_VSY,
				VOXEL_POLL_TIMEOUT_MS);
	mutex_unlock(&voxdev.lock);
	return ret;
}

static long voxel_ioc_begin_band(void __user *uarg)
{
	struct voxel_band_state band;
	int ret;

	if (copy_from_user(&band, uarg, sizeof(band)))
		return -EFAULT;
	if (band.band_index > 7)
		return -EINVAL;
	if (band.flush_x_min >= VOXEL_RENDER_WIDTH ||
	    band.flush_x_max >= VOXEL_RENDER_WIDTH ||
	    band.flush_x_min > band.flush_x_max)
		return -EINVAL;
	if (band.flush_y_min >= VOXEL_BAND_CACHE_HEIGHT ||
	    band.flush_y_max >= VOXEL_BAND_CACHE_HEIGHT ||
	    band.flush_y_min > band.flush_y_max)
		return -EINVAL;

	mutex_lock(&voxdev.lock);

	ret = voxel_poll_status(VOXEL_STAT_FEM, VOXEL_STAT_FEM,
				VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;
	ret = voxel_poll_status(VOXEL_STAT_BSY, 0, VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;

	voxel_wr(VOXEL_REG_BAND_INDEX, band.band_index);
	voxel_wr(VOXEL_REG_BAND_WINDOW,
		 (band.flush_x_min & 0x3ffu) |
		 ((band.flush_x_max & 0x3ffu) << 10) |
		 ((band.flush_y_min & 0x3fu) << 20) |
		 ((band.flush_y_max & 0x3fu) << 26));
	voxel_wr(VOXEL_REG_BAND_CTRL, VOXEL_BAND_CTRL_BEGIN);
	/*
	 * Do not wait for the band cache init here. The command FIFO can be
	 * filled while hardware clears/initializes the resident band; END_BAND
	 * still waits for FIFO-empty + idle before it requests the flush. The
	 * readback keeps the BAND_INDEX/BAND_CTRL writes ordered before the
	 * userspace write() that streams descriptors immediately after this ioctl.
	 */
	voxel_rd(VOXEL_REG_BAND_CTRL);

out:
	mutex_unlock(&voxdev.lock);
	return ret;
}

static long voxel_ioc_end_band(void)
{
	int ret;

	mutex_lock(&voxdev.lock);

	ret = voxel_poll_status(VOXEL_STAT_FEM, VOXEL_STAT_FEM,
				VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;
	ret = voxel_poll_status(VOXEL_STAT_BSY, 0, VOXEL_POLL_TIMEOUT_MS);
	if (ret)
		goto out;

	voxel_wr(VOXEL_REG_BAND_CTRL, VOXEL_BAND_CTRL_FLUSH);
	/*
	 * Do not poll BSY here. The background flush runs independently
	 * via flush_active; band_flush_pending is no longer in engine_busy
	 * so BSY clears immediately. The FSM priority chain ensures the
	 * flush completes before the next BEGIN_BAND starts drawing.
	 */

out:
	mutex_unlock(&voxdev.lock);
	return ret;
}

static long voxel_ioc_set_palette(void __user *uarg)
{
	struct voxel_palette_entry e;
	u32 data;

	if (copy_from_user(&e, uarg, sizeof(e)))
		return -EFAULT;

	data = ((u32)e.r << 16) | ((u32)e.g << 8) | (u32)e.b;

	mutex_lock(&voxdev.lock);
	voxel_wr(VOXEL_REG_PALETTE_ADDR, e.index);
	voxel_wr(VOXEL_REG_PALETTE_DATA, data);
	mutex_unlock(&voxdev.lock);

	return 0;
}

static long voxel_ioc_set_sky_palette(void __user *uarg)
{
	struct voxel_sky_palette_entry e;
	u32 data;

	if (copy_from_user(&e, uarg, sizeof(e)))
		return -EFAULT;

	data = ((u32)e.r << 16) | ((u32)e.g << 8) | (u32)e.b;

	mutex_lock(&voxdev.lock);
	voxel_wr(VOXEL_REG_SKY_PALETTE_ADDR, e.index);
	voxel_wr(VOXEL_REG_SKY_PALETTE_DATA, data);
	mutex_unlock(&voxdev.lock);

	return 0;
}

static long voxel_ioc_get_status(void __user *uarg)
{
	struct voxel_status s;
	u32 raw = voxel_status();

	s.raw        = raw;
	s.fifo_count = (raw >> VOXEL_STAT_FIFO_SHIFT) & VOXEL_STAT_FIFO_MASK;
	s.busy       = !!(raw & VOXEL_STAT_BSY);
	s.fifo_full  = !!(raw & VOXEL_STAT_FFL);
	s.fifo_empty = !!(raw & VOXEL_STAT_FEM);
	s.vsync      = !!(raw & VOXEL_STAT_VSY);

	if (copy_to_user(uarg, &s, sizeof(s)))
		return -EFAULT;
	return 0;
}

static long voxel_ioc_get_frame_count(void __user *uarg)
{
	u32 frames = voxel_rd(VOXEL_REG_FRAME_COUNT);

	if (copy_to_user(uarg, &frames, sizeof(frames)))
		return -EFAULT;
	return 0;
}

static long voxel_ioc_set_fog(void __user *uarg)
{
	struct voxel_fog_state fog;
	u32 range_word;
	u32 ctrl_word;

	if (copy_from_user(&fog, uarg, sizeof(fog)))
		return -EFAULT;

	range_word = ((u32)fog.end_dist << 16) | (u32)fog.start_dist;
	ctrl_word = ((u32)fog.inv_proj_sq << 16) |
		    ((fog.enabled ? 1u : 0u) << 8) |
		    (u32)fog.color_index;

	mutex_lock(&voxdev.lock);
	voxel_wr(VOXEL_REG_FOG_RANGE, range_word);
	voxel_wr(VOXEL_REG_FOG_CTRL, ctrl_word);
	mutex_unlock(&voxdev.lock);

	return 0;
}

static long voxel_ioc_set_extmem(void __user *uarg)
{
	struct voxel_extmem_state ext;

	if (copy_from_user(&ext, uarg, sizeof(ext)))
		return -EFAULT;

	mutex_lock(&voxdev.lock);
	voxel_wr(VOXEL_REG_EXTMEM_CTRL, ext.ctrl);
	voxel_wr(VOXEL_REG_EXTMEM_FRONT, ext.front_base);
	voxel_wr(VOXEL_REG_EXTMEM_BACK, ext.back_base);
	voxel_wr(VOXEL_REG_EXTMEM_STRIDE, ext.stride_bytes);
	voxel_wr(VOXEL_REG_EXTMEM_TILE, ext.tile_cfg);
	mutex_unlock(&voxdev.lock);

	return 0;
}

static long voxel_ioc_get_extmem(void __user *uarg)
{
	struct voxel_extmem_state ext;

	mutex_lock(&voxdev.lock);
	ext.ctrl = voxel_rd(VOXEL_REG_EXTMEM_CTRL);
	ext.front_base = voxel_rd(VOXEL_REG_EXTMEM_FRONT);
	ext.back_base = voxel_rd(VOXEL_REG_EXTMEM_BACK);
	ext.stride_bytes = voxel_rd(VOXEL_REG_EXTMEM_STRIDE);
	ext.tile_cfg = voxel_rd(VOXEL_REG_EXTMEM_TILE);
	ext.dma_status = voxel_rd(VOXEL_REG_EXTMEM_STAT);
	mutex_unlock(&voxdev.lock);

	if (copy_to_user(uarg, &ext, sizeof(ext)))
		return -EFAULT;
	return 0;
}

static long voxel_ioc_get_perf(void __user *uarg)
{
	struct voxel_perf_counters p;

	mutex_lock(&voxdev.lock);
	p.draw_active  = voxel_rd(VOXEL_REG_PERF_DRAW_ACT);
	p.draw_idle    = voxel_rd(VOXEL_REG_PERF_DRAW_IDLE);
	p.flush_active = voxel_rd(VOXEL_REG_PERF_FLUSH_ACT);
	p.flush_stall  = voxel_rd(VOXEL_REG_PERF_FLUSH_STL);
	p.init         = voxel_rd(VOXEL_REG_PERF_INIT);
	p.load         = voxel_rd(VOXEL_REG_PERF_LOAD);
	mutex_unlock(&voxdev.lock);

	if (copy_to_user(uarg, &p, sizeof(p)))
		return -EFAULT;
	return 0;
}

static long voxel_ioc_get_perf2(void __user *uarg)
{
	struct voxel_perf_counters_v2 p;

	mutex_lock(&voxdev.lock);
	p.base.draw_active  = voxel_rd(VOXEL_REG_PERF_DRAW_ACT);
	p.base.draw_idle    = voxel_rd(VOXEL_REG_PERF_DRAW_IDLE);
	p.base.flush_active = voxel_rd(VOXEL_REG_PERF_FLUSH_ACT);
	p.base.flush_stall  = voxel_rd(VOXEL_REG_PERF_FLUSH_STL);
	p.base.init         = voxel_rd(VOXEL_REG_PERF_INIT);
	p.base.load         = voxel_rd(VOXEL_REG_PERF_LOAD);
	p.flush_wait_load   = voxel_rd(VOXEL_REG_PERF_FLUSH_LOAD);
	p.flush_wait_fifo   = voxel_rd(VOXEL_REG_PERF_FLUSH_FIFO);
	p.flush_wait_data   = voxel_rd(VOXEL_REG_PERF_FLUSH_DATA);
	p.flush_wait_drain  = voxel_rd(VOXEL_REG_PERF_FLUSH_DRAIN);
	mutex_unlock(&voxdev.lock);

	if (copy_to_user(uarg, &p, sizeof(p)))
		return -EFAULT;
	return 0;
}

static long voxel_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != VOXEL_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) == 0 || _IOC_NR(cmd) > VOXEL_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case VOXEL_IOC_CLEAR_FRAME:
		return voxel_ioc_clear();
	case VOXEL_IOC_FLIP:
		return voxel_ioc_flip();
	case VOXEL_IOC_FLIP_ASYNC:
		return voxel_ioc_flip_async();
	case VOXEL_IOC_WAIT_FLIP:
		return voxel_ioc_wait_flip();
	case VOXEL_IOC_SET_PALETTE:
		return voxel_ioc_set_palette((void __user *)arg);
	case VOXEL_IOC_SET_SKY_PALETTE:
		return voxel_ioc_set_sky_palette((void __user *)arg);
	case VOXEL_IOC_GET_STATUS:
		return voxel_ioc_get_status((void __user *)arg);
	case VOXEL_IOC_GET_FRAME_COUNT:
		return voxel_ioc_get_frame_count((void __user *)arg);
	case VOXEL_IOC_SET_FOG:
		return voxel_ioc_set_fog((void __user *)arg);
	case VOXEL_IOC_SET_EXTMEM:
		return voxel_ioc_set_extmem((void __user *)arg);
	case VOXEL_IOC_GET_EXTMEM:
		return voxel_ioc_get_extmem((void __user *)arg);
	case VOXEL_IOC_BEGIN_BAND:
		return voxel_ioc_begin_band((void __user *)arg);
	case VOXEL_IOC_END_BAND:
		return voxel_ioc_end_band();
	case VOXEL_IOC_GET_PERF:
		return voxel_ioc_get_perf((void __user *)arg);
	case VOXEL_IOC_GET_PERF2:
		return voxel_ioc_get_perf2((void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations voxel_fops = {
	.owner          = THIS_MODULE,
	.open           = voxel_open,
	.release        = voxel_release,
	.write          = voxel_write,
	.unlocked_ioctl = voxel_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice voxel_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DRIVER_NAME,
	.fops  = &voxel_fops,
	.mode  = 0666,
};

/* ---------- platform driver glue ---------- */

static int voxel_probe(struct platform_device *pdev)
{
	int ret;

	mutex_init(&voxdev.lock);
	voxdev.bounce = kmalloc(VOXEL_BOUNCE_BYTES, GFP_KERNEL);
	if (!voxdev.bounce)
		return -ENOMEM;

	ret = misc_register(&voxel_miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_bounce;
	}

	ret = of_address_to_resource(pdev->dev.of_node, 0, &voxdev.res);
	if (ret) {
		dev_err(&pdev->dev, "of_address_to_resource failed\n");
		ret = -ENOENT;
		goto err_misc;
	}

	if (!request_mem_region(voxdev.res.start, resource_size(&voxdev.res),
				DRIVER_NAME)) {
		dev_err(&pdev->dev, "request_mem_region failed\n");
		ret = -EBUSY;
		goto err_misc;
	}

	voxdev.base = of_iomap(pdev->dev.of_node, 0);
	if (!voxdev.base) {
		dev_err(&pdev->dev, "of_iomap failed\n");
		ret = -ENOMEM;
		goto err_release;
	}

	/* Bring the engine up: enable, no interrupts, no pending pulses. */
	voxel_wr(VOXEL_REG_CONTROL, VOXEL_CTRL_EN);

	dev_info(&pdev->dev,
		 "voxel_gpu probed @ %pa, %llu bytes; status=0x%08x\n",
		 &voxdev.res.start,
		 (unsigned long long)resource_size(&voxdev.res),
		 voxel_status());
	return 0;

err_release:
	release_mem_region(voxdev.res.start, resource_size(&voxdev.res));
err_misc:
	misc_deregister(&voxel_miscdev);
err_bounce:
	kfree(voxdev.bounce);
	voxdev.bounce = NULL;
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void voxel_remove(struct platform_device *pdev)
#else
static int voxel_remove(struct platform_device *pdev)
#endif
{
	(void)pdev;

	/* Disable the engine before tearing down. */
	voxel_wr(VOXEL_REG_CONTROL, 0);

	iounmap(voxdev.base);
	release_mem_region(voxdev.res.start, resource_size(&voxdev.res));
	misc_deregister(&voxel_miscdev);
	kfree(voxdev.bounce);
	voxdev.bounce = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

#ifdef CONFIG_OF
static const struct of_device_id voxel_of_match[] = {
	{ .compatible = "csee4840,voxel_gpu-1.0" },
	{},
};
MODULE_DEVICE_TABLE(of, voxel_of_match);
#endif

static struct platform_driver voxel_driver = {
	.driver = {
		.name           = DRIVER_NAME,
		.owner          = THIS_MODULE,
		.of_match_table = of_match_ptr(voxel_of_match),
	},
	.remove = __exit_p(voxel_remove),
};

static int __init voxel_init(void)
{
	pr_info(DRIVER_NAME ": init\n");
	return platform_driver_probe(&voxel_driver, voxel_probe);
}

static void __exit voxel_exit(void)
{
	platform_driver_unregister(&voxel_driver);
	pr_info(DRIVER_NAME ": exit\n");
}

module_init(voxel_init);
module_exit(voxel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("the gang and twin");
MODULE_DESCRIPTION("Voxel GPU MVP driver — FIFO streaming + control ioctls");
