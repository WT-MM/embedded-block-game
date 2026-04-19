/*
 * voxel_gpu.c — MVP device driver for the FPGA voxel GPU peripheral.
 *
 * Responsibilities (per the MVP spec):
 *   (A) Map the FPGA registers exposed through the lightweight HPS bridge.
 *   (B) Expose /dev/voxel_gpu via the misc subsystem.
 *   (C) Stream user-space command bytes into the on-chip FIFO via write().
 *   (D) Provide the five MVP ioctls: CLEAR_FRAME, FLIP, SET_PALETTE,
 *       GET_STATUS, GET_FRAME_COUNT.
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
#include <linux/version.h>

#include "voxel_gpu.h"

#define DRIVER_NAME "voxel_gpu"

/*
 * Bounce buffer for write(): we copy from user in chunks, then push the
 * chunk word-by-word into the FIFO. Keep it well under the 8 KB FIFO so
 * we always have room to drain back-pressure between chunks.
 */
#define VOXEL_BOUNCE_WORDS  256                       /* 1 KB per chunk */
#define VOXEL_BOUNCE_BYTES  (VOXEL_BOUNCE_WORDS * 4)

/*
 * Polling timeouts. These are very generous — at 60 Hz a vsync arrives
 * every ~16 ms; the rasterizer should never take a full second on any
 * well-formed frame, so a 250 ms timeout is essentially "the hardware
 * is wedged".
 */
#define VOXEL_POLL_TIMEOUT_MS   250
#define VOXEL_POLL_DELAY_US     10

struct voxel_gpu_dev {
	struct resource res;
	void __iomem   *base;
	struct mutex    lock;       /* serializes register access */
};

static struct voxel_gpu_dev voxdev;

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

/*
 * Push a single 32-bit word, honoring back-pressure by polling FFL.
 * The Avalon slave will also stall the bus via waitrequest if we ignore
 * this, but explicit polling lets us bail out cleanly on a wedged GPU.
 */
static int voxel_fifo_push(u32 word)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(VOXEL_POLL_TIMEOUT_MS);

	while (voxel_status() & VOXEL_STAT_FFL) {
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		udelay(VOXEL_POLL_DELAY_US);
		cond_resched();
	}
	voxel_wr(VOXEL_FIFO_BASE, word);
	return 0;
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

	bounce = kmalloc(VOXEL_BOUNCE_BYTES, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	if (mutex_lock_interruptible(&voxdev.lock)) {
		kfree(bounce);
		return -ERESTARTSYS;
	}

	while (total < count) {
		size_t chunk = min_t(size_t, count - total, VOXEL_BOUNCE_BYTES);
		size_t i;

		if (copy_from_user(bounce, buf + total, chunk)) {
			ret = -EFAULT;
			break;
		}

		for (i = 0; i < chunk / 4; i++) {
			ret = voxel_fifo_push(bounce[i]);
			if (ret)
				goto out;
		}
		total += chunk;
	}

out:
	mutex_unlock(&voxdev.lock);
	kfree(bounce);

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

static long voxel_ioc_flip(void)
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

	/* Block until the next vsync pulse latches. */
	ret = voxel_poll_status(VOXEL_STAT_VSY, VOXEL_STAT_VSY,
				VOXEL_POLL_TIMEOUT_MS);

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
	case VOXEL_IOC_SET_PALETTE:
		return voxel_ioc_set_palette((void __user *)arg);
	case VOXEL_IOC_GET_STATUS:
		return voxel_ioc_get_status((void __user *)arg);
	case VOXEL_IOC_GET_FRAME_COUNT:
		return voxel_ioc_get_frame_count((void __user *)arg);
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

	ret = misc_register(&voxel_miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		return ret;
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
