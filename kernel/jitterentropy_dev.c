// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * Linux kernel module exposing the userspace jitterentropy library as
 * character devices and a hwrng backend.
 *
 * It provides:
 *   - /dev/jitterentropy        a character device backed by a single,
 *                               module-global Jitter RNG instance shared by
 *                               all readers (serialized with a mutex);
 *   - /dev/jitterentropy-multi  a character device that allocates an
 *                               independent Jitter RNG instance per open file
 *                               descriptor and frees it on close;
 *   - a hwrng backend named "jitterentropy" using a dedicated instance;
 *   - the JENT_IOC_STATUS ioctl returning the JSON status of the instance
 *     backing the file descriptor it is issued on.
 *
 * The oversampling rate (osr) and the JENT_* flags bitmask are exposed as
 * module parameters. This build uses the hardware time stamp noise source
 * only; the internal timer thread is not compiled in (see Makefile.kernel),
 * so JENT_FORCE_INTERNAL_TIMER is rejected.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"
#include "jitterentropy_uapi.h"

/* Size of the kernel bounce buffer used when copying entropy to userspace. */
#define JENT_READ_CHUNK 256

static unsigned int jent_osr = 1;
module_param_named(osr, jent_osr, uint, 0444);
MODULE_PARM_DESC(osr, "Oversampling rate for the Jitter RNG (default: 1)");

static unsigned int jent_flags;
module_param_named(flags, jent_flags, uint, 0444);
MODULE_PARM_DESC(flags,
	"JENT_* flags bitmask passed to jent_entropy_collector_alloc() (default: 0)");

/*
 * Effective flags actually handed to the library: the user-supplied jent_flags
 * with the internal-timer adjustments applied (see jent_mod_init()). Kept
 * separate so the flags module parameter keeps reflecting what the user set.
 */
static unsigned int jent_effective_flags;

static unsigned int jent_quality;
module_param_named(quality, jent_quality, uint, 0444);
MODULE_PARM_DESC(quality,
	"hwrng entropy quality in bits per 1024 bits of output; 0 (default) does not feed the kernel entropy pool");

/* Single shared instance backing /dev/jitterentropy. */
static struct rand_data *jent_single_ec;
static DEFINE_MUTEX(jent_single_lock);

/* Dedicated instance backing the hwrng device. */
static struct rand_data *jent_hwrng_ec;
static DEFINE_MUTEX(jent_hwrng_lock);

/* Per-open-file context for /dev/jitterentropy-multi. */
struct jent_multi_ctx {
	struct rand_data *ec;
	struct mutex lock;
};

/*
 * Copy up to @len bytes of conditioned entropy from the instance referenced
 * by @ec to userspace. @ec is passed by pointer so jent_read_entropy_safe()
 * can transparently reallocate the instance after a permanent health failure.
 */
static ssize_t jent_read_common(struct rand_data **ec, struct mutex *lock,
				char __user *ubuf, size_t len)
{
	u8 buf[JENT_READ_CHUNK];
	size_t done = 0;

	while (done < len) {
		size_t chunk = min(len - done, sizeof(buf));
		ssize_t rc;

		if (mutex_lock_interruptible(lock)) {
			if (done)
				break;
			return -ERESTARTSYS;
		}
		rc = jent_read_entropy_safe(ec, (char *)buf, chunk);
		mutex_unlock(lock);

		if (rc <= 0) {
			if (done)
				break;
			return -EIO;
		}

		if (copy_to_user(ubuf + done, buf, (size_t)rc)) {
			memzero_explicit(buf, sizeof(buf));
			return done ? (ssize_t)done : -EFAULT;
		}
		done += (size_t)rc;

		cond_resched();
		if (signal_pending(current)) {
			if (!done) {
				memzero_explicit(buf, sizeof(buf));
				return -ERESTARTSYS;
			}
			break;
		}
	}

	memzero_explicit(buf, sizeof(buf));
	return (ssize_t)done;
}

/*
 * Shared ioctl handler. @ec / @lock identify the Jitter RNG instance the
 * file descriptor is bound to.
 */
static long jent_ioctl_common(struct rand_data *ec, struct mutex *lock,
			      unsigned int cmd, unsigned long arg)
{
	struct jent_status *st;
	long ret = 0;
	int rc;

	switch (cmd) {
	case JENT_IOC_STATUS:
		st = kzalloc(sizeof(*st), GFP_KERNEL);
		if (!st)
			return -ENOMEM;

		if (mutex_lock_interruptible(lock)) {
			kfree(st);
			return -ERESTARTSYS;
		}
		rc = jent_status(ec, st->json, sizeof(st->json));
		mutex_unlock(lock);

		if (rc) {
			kfree(st);
			return -EIO;
		}

		st->length = (__u32)strnlen(st->json, sizeof(st->json));
		if (copy_to_user((void __user *)arg, st, sizeof(*st)))
			ret = -EFAULT;

		kfree(st);
		return ret;
	default:
		return -ENOTTY;
	}
}

/* --- /dev/jitterentropy: single shared instance --- */

static ssize_t jent_single_read(struct file *file, char __user *ubuf,
				size_t len, loff_t *ppos)
{
	return jent_read_common(&jent_single_ec, &jent_single_lock, ubuf, len);
}

static long jent_single_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	return jent_ioctl_common(jent_single_ec, &jent_single_lock, cmd, arg);
}

static const struct file_operations jent_single_fops = {
	.owner		= THIS_MODULE,
	.read		= jent_single_read,
	.unlocked_ioctl	= jent_single_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice jent_single_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "jitterentropy",
	.fops	= &jent_single_fops,
	.mode	= 0444,
};

/* --- /dev/jitterentropy-multi: one instance per open fd --- */

static int jent_multi_open(struct inode *inode, struct file *file)
{
	struct jent_multi_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	ctx->ec = jent_entropy_collector_alloc(jent_osr, jent_effective_flags);
	if (!ctx->ec) {
		mutex_destroy(&ctx->lock);
		kfree(ctx);
		return -EAGAIN;
	}

	file->private_data = ctx;
	return 0;
}

static int jent_multi_release(struct inode *inode, struct file *file)
{
	struct jent_multi_ctx *ctx = file->private_data;

	if (ctx) {
		jent_entropy_collector_free(ctx->ec);
		mutex_destroy(&ctx->lock);
		kfree(ctx);
	}
	return 0;
}

static ssize_t jent_multi_read(struct file *file, char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	struct jent_multi_ctx *ctx = file->private_data;

	return jent_read_common(&ctx->ec, &ctx->lock, ubuf, len);
}

static long jent_multi_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct jent_multi_ctx *ctx = file->private_data;

	return jent_ioctl_common(ctx->ec, &ctx->lock, cmd, arg);
}

static const struct file_operations jent_multi_fops = {
	.owner		= THIS_MODULE,
	.open		= jent_multi_open,
	.release	= jent_multi_release,
	.read		= jent_multi_read,
	.unlocked_ioctl	= jent_multi_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice jent_multi_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "jitterentropy-multi",
	.fops	= &jent_multi_fops,
	.mode	= 0444,
};

/* --- hwrng backend --- */

static int jent_hwrng_read(struct hwrng *rng, void *data, size_t max,
			   bool wait)
{
	ssize_t rc;

	if (mutex_lock_interruptible(&jent_hwrng_lock))
		return -ERESTARTSYS;
	rc = jent_read_entropy_safe(&jent_hwrng_ec, data, max);
	mutex_unlock(&jent_hwrng_lock);

	if (rc <= 0)
		return -EIO;
	return (int)rc;
}

static struct hwrng jent_hwrng = {
	.name	= "jitterentropy",
	.read	= jent_hwrng_read,
};

static int __init jent_mod_init(void)
{
	int ret;

	/*
	 * This module is built without the internal timer thread, so it can
	 * only operate the hardware time stamp noise source. Reject a request
	 * to force the (unavailable) internal timer and pin the configuration
	 * to the hardware time stamp explicitly.
	 */
	jent_effective_flags = jent_flags;
	if (jent_effective_flags & JENT_FORCE_INTERNAL_TIMER) {
		pr_warn("jitterentropy: internal timer is not available in this build; clearing JENT_FORCE_INTERNAL_TIMER\n");
		jent_effective_flags &= ~JENT_FORCE_INTERNAL_TIMER;
	}
	jent_effective_flags |= JENT_DISABLE_INTERNAL_TIMER;

	ret = jent_entropy_init_ex(jent_osr, jent_effective_flags);
	if (ret) {
		pr_err("jitterentropy: initialization/self test failed: %d\n",
		       ret);
		return -EIO;
	}

	jent_single_ec = jent_entropy_collector_alloc(jent_osr,
						      jent_effective_flags);
	if (!jent_single_ec)
		return -ENOMEM;

	jent_hwrng_ec = jent_entropy_collector_alloc(jent_osr,
						     jent_effective_flags);
	if (!jent_hwrng_ec) {
		ret = -ENOMEM;
		goto err_free_single;
	}

	jent_hwrng.quality = (unsigned short)min(jent_quality, 1024u);

	ret = misc_register(&jent_single_dev);
	if (ret)
		goto err_free_hwrng;

	ret = misc_register(&jent_multi_dev);
	if (ret)
		goto err_dereg_single;

	ret = hwrng_register(&jent_hwrng);
	if (ret)
		goto err_dereg_multi;

	pr_info("jitterentropy: loaded (version %u, osr=%u, flags=0x%x, hwrng quality=%u)\n",
		jent_version(), jent_osr, jent_flags, jent_hwrng.quality);
	return 0;

err_dereg_multi:
	misc_deregister(&jent_multi_dev);
err_dereg_single:
	misc_deregister(&jent_single_dev);
err_free_hwrng:
	jent_entropy_collector_free(jent_hwrng_ec);
err_free_single:
	jent_entropy_collector_free(jent_single_ec);
	return ret;
}

static void __exit jent_mod_exit(void)
{
	hwrng_unregister(&jent_hwrng);
	misc_deregister(&jent_multi_dev);
	misc_deregister(&jent_single_dev);
	jent_entropy_collector_free(jent_hwrng_ec);
	jent_entropy_collector_free(jent_single_ec);
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Jitter RNG character devices and hwrng backend");
