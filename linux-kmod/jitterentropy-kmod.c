// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Linux kernel module shim for the jitterentropy noise source.
 *
 * The actual entropy collection logic lives in src/ and is built into
 * this module unmodified. This file only adds the kernel-side glue:
 *
 *   - a hwrng device feeding the kernel's /dev/hwrng
 *   - a misc character device /dev/jitterentropy; each open() allocates
 *     a private struct rand_data that backs read() and is released on
 *     close()
 *   - a misc character device /dev/jitterentropy-status; open() snapshots
 *     the JSON status of the shared hwrng collector and read() serves
 *     that snapshot back
 *
 * Module parameters select the OSR and feature flags applied to every
 * collector this module allocates (the shared hwrng collector and each
 * per-open /dev/jitterentropy collector). NTG.1 mode is exposed as a
 * separate bool because it is the most common reason to override flags.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hw_random.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"

#define JENT_MODNAME "jitterentropy"

static unsigned int jent_osr;
module_param_named(osr, jent_osr, uint, 0644);
MODULE_PARM_DESC(osr,
	"Oversampling rate passed to jent_entropy_collector_alloc(); 0 lets the library pick its minimum.");

static unsigned int jent_flags;
module_param_named(flags, jent_flags, uint, 0644);
MODULE_PARM_DESC(flags,
	"Raw flag bits passed to jent_entropy_collector_alloc(); see JENT_* in jitterentropy.h.");

static bool jent_ntg1;
module_param_named(ntg1, jent_ntg1, bool, 0644);
MODULE_PARM_DESC(ntg1,
	"Enable AIS 20/31 NTG.1 mode (sets JENT_NTG1 on every allocated collector).");

static inline unsigned int jent_effective_flags(void)
{
	unsigned int f = jent_flags;

	if (jent_ntg1)
		f |= JENT_NTG1;
	return f;
}

/* ------------------------------------------------------------------ */
/* hwrng back-end: shared collector serialized by a mutex.            */
/* ------------------------------------------------------------------ */

static struct rand_data *jent_shared_ec;
static DEFINE_MUTEX(jent_shared_lock);

static int jent_hwrng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	ssize_t ret;

	(void)rng;
	(void)wait;
	if (max == 0)
		return 0;

	mutex_lock(&jent_shared_lock);
	if (!jent_shared_ec) {
		mutex_unlock(&jent_shared_lock);
		return -ENODEV;
	}
	ret = jent_read_entropy_safe(&jent_shared_ec, data, max);
	mutex_unlock(&jent_shared_lock);

	return (ret < 0) ? -EIO : (int)ret;
}

static struct hwrng jent_hwrng = {
	.name		= JENT_MODNAME,
	.read		= jent_hwrng_read,
	.quality	= 1024,
};

/* ------------------------------------------------------------------ */
/* /dev/jitterentropy: per-open struct rand_data.                     */
/* ------------------------------------------------------------------ */

static int jent_cdev_open(struct inode *inode, struct file *file)
{
	struct rand_data *ec;

	(void)inode;
	ec = jent_entropy_collector_alloc(jent_osr, jent_effective_flags());
	if (!ec)
		return -ENOMEM;
	file->private_data = ec;
	return 0;
}

static int jent_cdev_release(struct inode *inode, struct file *file)
{
	(void)inode;
	if (file->private_data) {
		jent_entropy_collector_free(file->private_data);
		file->private_data = NULL;
	}
	return 0;
}

static ssize_t jent_cdev_read(struct file *file, char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct rand_data **ec = (struct rand_data **)&file->private_data;
	size_t chunk;
	void *kbuf;
	ssize_t ret;

	(void)ppos;
	if (!*ec)
		return -ENODEV;
	if (len == 0)
		return 0;

	chunk = min(len, (size_t)PAGE_SIZE);
	kbuf = kmalloc(chunk, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	ret = jent_read_entropy_safe(ec, kbuf, chunk);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}
	if (copy_to_user(ubuf, kbuf, (size_t)ret))
		ret = -EFAULT;

out:
	memzero_explicit(kbuf, chunk);
	kfree(kbuf);
	return ret;
}

static const struct file_operations jent_cdev_fops = {
	.owner		= THIS_MODULE,
	.open		= jent_cdev_open,
	.release	= jent_cdev_release,
	.read		= jent_cdev_read,
	.llseek		= noop_llseek,
};

static struct miscdevice jent_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= JENT_MODNAME,
	.fops	= &jent_cdev_fops,
	.mode	= 0444,
};

/* ------------------------------------------------------------------ */
/* /dev/jitterentropy-status: JSON snapshot of the shared collector.  */
/* ------------------------------------------------------------------ */

#define JENT_STATUS_BUFSIZE PAGE_SIZE

struct jent_status_snapshot {
	size_t	len;
	char	buf[];
};

static int jent_status_open(struct inode *inode, struct file *file)
{
	struct jent_status_snapshot *snap;

	(void)inode;
	snap = kzalloc(sizeof(*snap) + JENT_STATUS_BUFSIZE, GFP_KERNEL);
	if (!snap)
		return -ENOMEM;

	mutex_lock(&jent_shared_lock);
	if (jent_status(jent_shared_ec, snap->buf, JENT_STATUS_BUFSIZE) < 0) {
		mutex_unlock(&jent_shared_lock);
		kfree(snap);
		return -EIO;
	}
	mutex_unlock(&jent_shared_lock);

	snap->len = strnlen(snap->buf, JENT_STATUS_BUFSIZE);
	file->private_data = snap;
	return 0;
}

static int jent_status_release(struct inode *inode, struct file *file)
{
	(void)inode;
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t jent_status_read(struct file *file, char __user *ubuf,
				size_t len, loff_t *ppos)
{
	struct jent_status_snapshot *snap = file->private_data;

	if (!snap)
		return -ENODEV;
	return simple_read_from_buffer(ubuf, len, ppos, snap->buf, snap->len);
}

static const struct file_operations jent_status_fops = {
	.owner		= THIS_MODULE,
	.open		= jent_status_open,
	.release	= jent_status_release,
	.read		= jent_status_read,
	.llseek		= default_llseek,
};

static struct miscdevice jent_status_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= JENT_MODNAME "-status",
	.fops	= &jent_status_fops,
	.mode	= 0444,
};

/* ------------------------------------------------------------------ */
/* Module init / exit.                                                */
/* ------------------------------------------------------------------ */

static int __init jent_module_init(void)
{
	unsigned int flags = jent_effective_flags();
	int ret;

	ret = jent_entropy_init_ex(jent_osr, flags);
	if (ret) {
		pr_err(JENT_MODNAME ": entropy init failed (osr=%u flags=0x%x ntg1=%d): %d\n",
		       jent_osr, flags, jent_ntg1, ret);
		return -EIO;
	}

	jent_shared_ec = jent_entropy_collector_alloc(jent_osr, flags);
	if (!jent_shared_ec) {
		pr_err(JENT_MODNAME ": shared collector alloc failed\n");
		return -ENOMEM;
	}

	ret = hwrng_register(&jent_hwrng);
	if (ret) {
		pr_err(JENT_MODNAME ": hwrng_register failed: %d\n", ret);
		goto err_hwrng;
	}

	ret = misc_register(&jent_misc);
	if (ret) {
		pr_err(JENT_MODNAME ": misc_register failed: %d\n", ret);
		goto err_misc;
	}

	ret = misc_register(&jent_status_misc);
	if (ret) {
		pr_err(JENT_MODNAME ": status misc_register failed: %d\n", ret);
		goto err_status;
	}

	pr_info(JENT_MODNAME ": loaded (lib %u, osr=%u flags=0x%x ntg1=%d)\n",
		jent_version(), jent_osr, flags, jent_ntg1);
	return 0;

err_status:
	misc_deregister(&jent_misc);
err_misc:
	hwrng_unregister(&jent_hwrng);
err_hwrng:
	jent_entropy_collector_free(jent_shared_ec);
	jent_shared_ec = NULL;
	return ret;
}

static void __exit jent_module_exit(void)
{
	misc_deregister(&jent_status_misc);
	misc_deregister(&jent_misc);
	hwrng_unregister(&jent_hwrng);

	mutex_lock(&jent_shared_lock);
	jent_entropy_collector_free(jent_shared_ec);
	jent_shared_ec = NULL;
	mutex_unlock(&jent_shared_lock);
}

module_init(jent_module_init);
module_exit(jent_module_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("CPU jitter entropy noise source (kernel module)");
