// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * jitterentropy_kmod - Linux kernel module wrapping the jitterentropy
 * library to expose it both as a hardware RNG (struct hwrng) and as a
 * character device under /dev/jitterentropy.
 *
 * The library itself is compiled with JENT_KERNEL from this Kbuild file
 * so that all libc dependencies disappear; only the noise-source logic
 * is linked into the module.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hw_random.h>
#include <linux/moduleparam.h>

#include "jitterentropy.h"
#include "jitterentropy_kmod.h"

/* --- module parameters --------------------------------------------------- */

static unsigned int osr;
module_param(osr, uint, 0444);
MODULE_PARM_DESC(osr,
	"Oversampling rate handed to jent_entropy_collector_alloc(). "
	"0 selects the library default (JENT_MIN_OSR = 3).");

static unsigned int flags;
module_param(flags, uint, 0444);
MODULE_PARM_DESC(flags,
	"Bitwise OR of JENT_* flags handed to jent_entropy_collector_alloc(). "
	"Recognised bits (jitterentropy 3.7.0): "
	"JENT_DISABLE_MEMORY_ACCESS (1<<2), "
	"JENT_FORCE_FIPS (1<<5), "
	"JENT_NTG1 (1<<6), "
	"JENT_CACHE_ALL (1<<7), "
	"plus the JENT_MAX_MEMSIZE_* and JENT_HASHLOOP_* memory/hash-loop "
	"selectors encoded in the upper bits.");

/* JENT_FORCE_INTERNAL_TIMER / JENT_DISABLE_INTERNAL_TIMER are rejected
 * at module load - the kernel build disables the internal-timer code
 * path entirely (no kthread is spawned) and always uses the high-
 * resolution timer the kernel already provides.
 */

/* --- module state -------------------------------------------------------- */

static DEFINE_MUTEX(jent_lock);
static struct rand_data *jent_ec;

/* --- core read helper ---------------------------------------------------- */

static ssize_t jent_kmod_read(void *out, size_t max, bool wait)
{
	ssize_t ret;

	(void)wait;

	if (max == 0)
		return 0;

	mutex_lock(&jent_lock);
	ret = jent_read_entropy_safe(&jent_ec, out, max);
	mutex_unlock(&jent_lock);

	if (ret < 0) {
		pr_warn_ratelimited(
			"jitterentropy_kmod: jent_read_entropy_safe failed: %zd\n",
			ret);
		return -EIO;
	}

	return ret;
}

/* --- hwrng glue ---------------------------------------------------------- */

static int jent_hwrng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	(void)rng;
	return (int)jent_kmod_read(data, max, wait);
}

static struct hwrng jent_hwrng = {
	.name = "jitterentropy",
	.read = jent_hwrng_read,
	/*
	 * Jitter RNG output is full-entropy after the conditioner, so report
	 * 1 bit of entropy per output bit (1000 per 1000-bit sample) to the
	 * core. This matches what the in-tree jitterentropy_rng driver does.
	 */
	.quality = 1000,
};

/* --- character device glue ----------------------------------------------- */

#define JENT_CHRDEV_CHUNK 256

static ssize_t jent_chrdev_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	unsigned char buf[JENT_CHRDEV_CHUNK];
	size_t copied = 0;

	(void)file;
	(void)ppos;

	while (copied < count) {
		size_t want = min_t(size_t, count - copied, sizeof(buf));
		ssize_t got = jent_kmod_read(buf, want, true);

		if (got <= 0) {
			memzero_explicit(buf, sizeof(buf));
			return copied ? (ssize_t)copied : got;
		}

		if (copy_to_user(ubuf + copied, buf, got)) {
			memzero_explicit(buf, sizeof(buf));
			return copied ? (ssize_t)copied : -EFAULT;
		}

		copied += got;

		if (need_resched())
			cond_resched();
	}

	memzero_explicit(buf, sizeof(buf));
	return copied;
}

static int jent_chrdev_open(struct inode *inode, struct file *file)
{
	/*
	 * Mark the file as non-seekable. nonseekable_open() clears
	 * FMODE_LSEEK on f_mode, so vfs_llseek() returns -ESPIPE without
	 * ever dispatching to a .llseek callback. This has been stable
	 * across the 2.6 / 3.x / 4.x / 5.x / 6.x kernel series and is the
	 * portable replacement for no_llseek (removed in v6.12).
	 */
	return nonseekable_open(inode, file);
}

static long jent_ioctl_status(unsigned long arg)
{
	struct jent_kmod_status_ioc ioc;
	void __user *uarg = (void __user *)arg;
	char *kbuf;
	int rc;
	size_t to_copy;

	if (copy_from_user(&ioc, uarg, sizeof(ioc)))
		return -EFAULT;

	if (ioc.flags != 0)
		return -EINVAL;
	if (ioc.buflen == 0 || ioc.buf == 0)
		return -EINVAL;
	if (ioc.buflen > JENT_KMOD_STATUS_MAX_BUFLEN)
		return -E2BIG;

	kbuf = kzalloc(ioc.buflen, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&jent_lock);
	if (!jent_ec) {
		mutex_unlock(&jent_lock);
		kfree(kbuf);
		return -ENODEV;
	}
	rc = jent_status(jent_ec, kbuf, ioc.buflen);
	mutex_unlock(&jent_lock);

	if (rc != 0) {
		/*
		 * jent_status returns -1 when the buffer is too small (or
		 * NULL / buflen == 0, both already rejected above).
		 */
		kfree(kbuf);
		return -ENOSPC;
	}

	/*
	 * jent_status writes a NUL-terminated string; report the number of
	 * bytes excluding the terminator so the user-space caller can use
	 * the returned length directly as a JSON-parser input.
	 */
	to_copy = strnlen(kbuf, ioc.buflen);

	if (copy_to_user((void __user *)(uintptr_t)ioc.buf, kbuf, to_copy)) {
		memzero_explicit(kbuf, ioc.buflen);
		kfree(kbuf);
		return -EFAULT;
	}

	/* Wipe before freeing - the status JSON may include flag values
	 * that an attacker shouldn't learn from a memory disclosure. */
	memzero_explicit(kbuf, ioc.buflen);
	kfree(kbuf);

	ioc.buflen = (u32)to_copy;
	if (copy_to_user(uarg, &ioc, sizeof(ioc)))
		return -EFAULT;

	return 0;
}

static long jent_chrdev_unlocked_ioctl(struct file *file, unsigned int cmd,
				       unsigned long arg)
{
	(void)file;

	switch (cmd) {
	case JENT_KMOD_IOC_STATUS:
		return jent_ioctl_status(arg);
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
/*
 * The ioctl struct uses fixed-width __u32 / __u64 fields so the layout
 * is identical for 32-bit and 64-bit user space; the same handler
 * works for both. Routing compat_ioctl through compat_ptr_ioctl()
 * applies the standard compat_ptr() sign-extension for the arg.
 */
#define JENT_CHRDEV_COMPAT_IOCTL  .compat_ioctl = compat_ptr_ioctl,
#else
#define JENT_CHRDEV_COMPAT_IOCTL
#endif

static const struct file_operations jent_chrdev_fops = {
	.owner          = THIS_MODULE,
	.open           = jent_chrdev_open,
	.read           = jent_chrdev_read,
	.unlocked_ioctl = jent_chrdev_unlocked_ioctl,
	JENT_CHRDEV_COMPAT_IOCTL
};

static struct miscdevice jent_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "jitterentropy",
	.fops  = &jent_chrdev_fops,
	.mode  = 0444,
};

/* --- /dev/jitterentropy-status (status JSON) ----------------------------- */

/*
 * jent_status() typically emits ~1.5 KiB of JSON; 8 KiB is a generous
 * upper bound that still fits comfortably in one allocation.
 */
#define JENT_STATUS_BUF_SIZE 8192

static ssize_t jent_status_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	size_t len;
	ssize_t copied;
	int rc;

	(void)file;

	/*
	 * Regenerate the status JSON on every read. The collector state
	 * may have evolved (health-test counters, OSR after a reset, ...)
	 * between two reads, so users observe the latest snapshot at the
	 * moment they ask for it - never a stale open-time copy.
	 *
	 * Side effect: a small-buffer reader that loops will see each
	 * iteration's JSON regenerated from a possibly different
	 * collector state. The conventional `cat` usage performs a
	 * single read(buf, 4096), which captures the JSON atomically.
	 */
	kbuf = kzalloc(JENT_STATUS_BUF_SIZE, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&jent_lock);
	if (!jent_ec) {
		mutex_unlock(&jent_lock);
		kfree(kbuf);
		return -ENODEV;
	}
	rc = jent_status(jent_ec, kbuf, JENT_STATUS_BUF_SIZE);
	mutex_unlock(&jent_lock);

	if (rc != 0) {
		/* Buffer too small - bump JENT_STATUS_BUF_SIZE if ever hit. */
		kfree(kbuf);
		return -EIO;
	}

	len = strnlen(kbuf, JENT_STATUS_BUF_SIZE);
	copied = simple_read_from_buffer(ubuf, count, ppos, kbuf, len);

	memzero_explicit(kbuf, JENT_STATUS_BUF_SIZE);
	kfree(kbuf);
	return copied;
}

static const struct file_operations jent_status_fops = {
	.owner  = THIS_MODULE,
	.read   = jent_status_read,
	.llseek = default_llseek,
};

static struct miscdevice jent_status_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "jitterentropy-status",
	.fops  = &jent_status_fops,
	.mode  = 0444,
};

/* --- init / exit --------------------------------------------------------- */

/*
 * Flags that don't make sense in the kernel build: the internal timer is
 * compiled out, so both forcing and disabling it are rejected.
 */
#define JENT_KMOD_REJECT_FLAGS \
	(JENT_FORCE_INTERNAL_TIMER | JENT_DISABLE_INTERNAL_TIMER)

static int __init jent_kmod_init(void)
{
	int ret;

	if (flags & JENT_KMOD_REJECT_FLAGS) {
		pr_err("jitterentropy_kmod: JENT_FORCE_INTERNAL_TIMER and "
		       "JENT_DISABLE_INTERNAL_TIMER are not meaningful in "
		       "kernel mode (flags=0x%x)\n", flags);
		return -EINVAL;
	}

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		pr_err("jitterentropy_kmod: jent_entropy_init_ex(osr=%u, "
		       "flags=0x%x) failed: %d\n", osr, flags, ret);
		return -EIO;
	}

	jent_ec = jent_entropy_collector_alloc(osr, flags);
	if (!jent_ec) {
		pr_err("jitterentropy_kmod: jent_entropy_collector_alloc(osr=%u, "
		       "flags=0x%x) returned NULL\n", osr, flags);
		return -ENOMEM;
	}

	ret = hwrng_register(&jent_hwrng);
	if (ret) {
		pr_err("jitterentropy_kmod: hwrng_register failed: %d\n", ret);
		goto err_free_ec;
	}

	ret = misc_register(&jent_miscdev);
	if (ret) {
		pr_err("jitterentropy_kmod: misc_register(%s) failed: %d\n",
		       jent_miscdev.name, ret);
		goto err_unreg_hwrng;
	}

	ret = misc_register(&jent_status_miscdev);
	if (ret) {
		pr_err("jitterentropy_kmod: misc_register(%s) failed: %d\n",
		       jent_status_miscdev.name, ret);
		goto err_unreg_miscdev;
	}

	pr_info("jitterentropy_kmod: registered hwrng=%s, /dev/%s and "
		"/dev/%s (osr=%u, flags=0x%x, lib v%u)\n",
		jent_hwrng.name, jent_miscdev.name, jent_status_miscdev.name,
		osr, flags, jent_version());

	return 0;

err_unreg_miscdev:
	misc_deregister(&jent_miscdev);
err_unreg_hwrng:
	hwrng_unregister(&jent_hwrng);
err_free_ec:
	jent_entropy_collector_free(jent_ec);
	jent_ec = NULL;
	return ret;
}

static void __exit jent_kmod_exit(void)
{
	misc_deregister(&jent_status_miscdev);
	misc_deregister(&jent_miscdev);
	hwrng_unregister(&jent_hwrng);

	mutex_lock(&jent_lock);
	jent_entropy_collector_free(jent_ec);
	jent_ec = NULL;
	mutex_unlock(&jent_lock);

	pr_info("jitterentropy_kmod: unloaded\n");
}

module_init(jent_kmod_init);
module_exit(jent_kmod_exit);

MODULE_AUTHOR("Markus Theil <theil.markus@gmail.com>");
MODULE_DESCRIPTION("hwrng + /dev/jitterentropy backed by the jitterentropy library");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("jitterentropy");
