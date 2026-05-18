/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * macOS (xnu) KEXT wrapping the jitterentropy library. Exposes a
 * /dev/jitterentropy character device returning random bytes from the
 * jitter RNG, plus an open-on-each-read /dev/jitterentropy-status that
 * yields the jent_status JSON.
 *
 * Caveats specific to this target:
 *
 *   - Apple deprecated third-party KEXTs starting with macOS 11. On
 *     Apple Silicon machines, loading this requires booting into
 *     "Reduced Security" mode and explicitly approving third-party
 *     kernel extensions. On Intel Macs, SIP must be lowered with
 *     `csrutil enable --without kext` (or `csrutil disable`).
 *   - Code signing with the "com.apple.developer.driverkit" /
 *     "Kernel Extension" entitlement is required for installation in
 *     /Library/Extensions on stock systems. For development work it is
 *     usually loaded out of /tmp via `kmutil load -p /tmp/...kext`.
 *
 * The companion macos/Makefile builds the bundle; macos/Info.plist
 * provides the kext metadata.
 */

#include <mach/mach_types.h>
#include <libkern/libkern.h>
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>

#include "jitterentropy.h"

/* --- module-wide state ---------------------------------------------------- */

static lck_grp_t *jent_lock_grp;
static lck_mtx_t *jent_lock;

static struct rand_data *jent_ec;

static int    jent_major       = -1;
static void  *jent_devnode;
static void  *jent_status_devnode;

#define JENT_CHRDEV_CHUNK     256
#define JENT_STATUS_BUF_SIZE  8192

/*
 * Flags that don't make sense in the kernel build - the internal-timer
 * code path is compiled out, so both forcing and disabling it are
 * rejected at load time.
 */
#define JENT_KMOD_REJECT_FLAGS \
	(JENT_FORCE_INTERNAL_TIMER | JENT_DISABLE_INTERNAL_TIMER)

/*
 * KEXTs cannot accept module parameters the way Linux kmods do. The
 * load script can however poke kernel symbols, so expose the chosen
 * osr / flags as global ints. By default the library picks JENT_MIN_OSR
 * = 3 and flags = 0.
 */
unsigned int jent_kmod_osr   = 0;
unsigned int jent_kmod_flags = 0;

/* --- /dev/jitterentropy (random bytes) ----------------------------------- */

static int
jent_chrdev_read(dev_t dev, struct uio *uio, int ioflag)
{
	unsigned char buf[JENT_CHRDEV_CHUNK];
	int error = 0;

	(void)dev;
	(void)ioflag;

	while (uio_resid(uio) > 0) {
		user_ssize_t resid = uio_resid(uio);
		size_t want = (size_t)((resid < (user_ssize_t)sizeof(buf)) ?
				       resid : (user_ssize_t)sizeof(buf));
		ssize_t got;

		lck_mtx_lock(jent_lock);
		got = jent_read_entropy_safe(&jent_ec, (char *)buf, want);
		lck_mtx_unlock(jent_lock);

		if (got <= 0) {
			bzero(buf, sizeof(buf));
			return EIO;
		}

		error = uiomove((const char *)buf, (int)got, uio);
		bzero(buf, sizeof(buf));
		if (error)
			return error;
	}

	return 0;
}

/* --- /dev/jitterentropy-status (live jent_status JSON) ------------------- */

static int
jent_status_read(dev_t dev, struct uio *uio, int ioflag)
{
	char  *kbuf;
	size_t len;
	off_t  pos;
	int    rc, error;

	(void)dev;
	(void)ioflag;

	kbuf = (char *)IOMalloc(JENT_STATUS_BUF_SIZE);
	if (!kbuf)
		return ENOMEM;
	bzero(kbuf, JENT_STATUS_BUF_SIZE);

	lck_mtx_lock(jent_lock);
	if (!jent_ec) {
		lck_mtx_unlock(jent_lock);
		IOFree(kbuf, JENT_STATUS_BUF_SIZE);
		return ENODEV;
	}
	rc = jent_status(jent_ec, kbuf, JENT_STATUS_BUF_SIZE);
	lck_mtx_unlock(jent_lock);

	if (rc != 0) {
		IOFree(kbuf, JENT_STATUS_BUF_SIZE);
		return EIO;
	}

	len = strnlen(kbuf, JENT_STATUS_BUF_SIZE);
	pos = uio_offset(uio);
	if (pos < 0 || (size_t)pos >= len) {
		IOFree(kbuf, JENT_STATUS_BUF_SIZE);
		return 0;	/* EOF */
	}

	error = uiomove(kbuf + pos, (int)(len - (size_t)pos), uio);
	bzero(kbuf, JENT_STATUS_BUF_SIZE);
	IOFree(kbuf, JENT_STATUS_BUF_SIZE);
	return error;
}

/* --- cdevsw + open/close stubs ------------------------------------------ */

static int jent_chrdev_open (dev_t dev, int flags, int devtype, proc_t p)
{ (void)dev; (void)flags; (void)devtype; (void)p; return 0; }

static int jent_chrdev_close(dev_t dev, int flags, int devtype, proc_t p)
{ (void)dev; (void)flags; (void)devtype; (void)p; return 0; }

static struct cdevsw jent_cdevsw = {
	.d_open    = jent_chrdev_open,
	.d_close   = jent_chrdev_close,
	.d_read    = jent_chrdev_read,
	.d_write   = eno_rdwrt,
	.d_ioctl   = eno_ioctl,
	.d_stop    = eno_stop,
	.d_reset   = eno_reset,
	.d_ttys    = NULL,
	.d_select  = eno_select,
	.d_mmap    = eno_mmap,
	.d_strategy= eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type    = 0,
};

static struct cdevsw jent_status_cdevsw = {
	.d_open    = jent_chrdev_open,
	.d_close   = jent_chrdev_close,
	.d_read    = jent_status_read,
	.d_write   = eno_rdwrt,
	.d_ioctl   = eno_ioctl,
	.d_stop    = eno_stop,
	.d_reset   = eno_reset,
	.d_ttys    = NULL,
	.d_select  = eno_select,
	.d_mmap    = eno_mmap,
	.d_strategy= eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type    = 0,
};

static int jent_status_major = -1;

/* --- kext lifecycle ------------------------------------------------------ */

kern_return_t jitterentropy_kmod_start(kmod_info_t *ki, void *d);
kern_return_t jitterentropy_kmod_stop (kmod_info_t *ki, void *d);

kern_return_t
jitterentropy_kmod_start(kmod_info_t *ki, void *d)
{
	int rc;

	(void)ki;
	(void)d;

	if (jent_kmod_flags & JENT_KMOD_REJECT_FLAGS) {
		printf("jitterentropy_kmod: JENT_FORCE_INTERNAL_TIMER and "
		       "JENT_DISABLE_INTERNAL_TIMER are not meaningful in "
		       "kernel mode (flags=0x%x)\n", jent_kmod_flags);
		return KERN_INVALID_ARGUMENT;
	}

	rc = jent_entropy_init_ex(jent_kmod_osr, jent_kmod_flags);
	if (rc) {
		printf("jitterentropy_kmod: jent_entropy_init_ex(osr=%u, "
		       "flags=0x%x) failed: %d\n",
		       jent_kmod_osr, jent_kmod_flags, rc);
		return KERN_FAILURE;
	}

	jent_lock_grp = lck_grp_alloc_init("jitterentropy", LCK_GRP_ATTR_NULL);
	if (!jent_lock_grp)
		return KERN_RESOURCE_SHORTAGE;
	jent_lock = lck_mtx_alloc_init(jent_lock_grp, LCK_ATTR_NULL);
	if (!jent_lock) {
		lck_grp_free(jent_lock_grp);
		jent_lock_grp = NULL;
		return KERN_RESOURCE_SHORTAGE;
	}

	jent_ec = jent_entropy_collector_alloc(jent_kmod_osr, jent_kmod_flags);
	if (!jent_ec)
		goto fail_lock;

	jent_major = cdevsw_add(-1, &jent_cdevsw);
	if (jent_major < 0)
		goto fail_ec;

	jent_status_major = cdevsw_add(-1, &jent_status_cdevsw);
	if (jent_status_major < 0)
		goto fail_major;

	jent_devnode = devfs_make_node(makedev(jent_major, 0),
				       DEVFS_CHAR, 0 /* UID_ROOT */,
				       0 /* GID_WHEEL */, 0444,
				       "jitterentropy");
	if (!jent_devnode)
		goto fail_status_major;

	jent_status_devnode = devfs_make_node(
		makedev(jent_status_major, 0),
		DEVFS_CHAR, 0, 0, 0444, "jitterentropy-status");
	if (!jent_status_devnode)
		goto fail_devnode;

	printf("jitterentropy_kmod: loaded - /dev/jitterentropy + "
	       "/dev/jitterentropy-status (osr=%u, flags=0x%x, lib v%u)\n",
	       jent_kmod_osr, jent_kmod_flags, jent_version());
	return KERN_SUCCESS;

fail_devnode:
	devfs_remove(jent_devnode);
	jent_devnode = NULL;
fail_status_major:
	(void)cdevsw_remove(jent_status_major, &jent_status_cdevsw);
	jent_status_major = -1;
fail_major:
	(void)cdevsw_remove(jent_major, &jent_cdevsw);
	jent_major = -1;
fail_ec:
	jent_entropy_collector_free(jent_ec);
	jent_ec = NULL;
fail_lock:
	lck_mtx_free(jent_lock, jent_lock_grp);
	jent_lock = NULL;
	lck_grp_free(jent_lock_grp);
	jent_lock_grp = NULL;
	return KERN_FAILURE;
}

kern_return_t
jitterentropy_kmod_stop(kmod_info_t *ki, void *d)
{
	(void)ki;
	(void)d;

	if (jent_status_devnode) {
		devfs_remove(jent_status_devnode);
		jent_status_devnode = NULL;
	}
	if (jent_devnode) {
		devfs_remove(jent_devnode);
		jent_devnode = NULL;
	}
	if (jent_status_major >= 0) {
		(void)cdevsw_remove(jent_status_major, &jent_status_cdevsw);
		jent_status_major = -1;
	}
	if (jent_major >= 0) {
		(void)cdevsw_remove(jent_major, &jent_cdevsw);
		jent_major = -1;
	}

	if (jent_lock)
		lck_mtx_lock(jent_lock);
	jent_entropy_collector_free(jent_ec);
	jent_ec = NULL;
	if (jent_lock) {
		lck_mtx_unlock(jent_lock);
		lck_mtx_free(jent_lock, jent_lock_grp);
		jent_lock = NULL;
	}
	if (jent_lock_grp) {
		lck_grp_free(jent_lock_grp);
		jent_lock_grp = NULL;
	}

	printf("jitterentropy_kmod: unloaded\n");
	return KERN_SUCCESS;
}

/*
 * KMOD_EXPLICIT_DECL plus the trailing _realmain / _antimain /
 * _kext_apple_cc are the boilerplate every plain-C KEXT needs so that
 * kxld can resolve the bundle's entry points.
 */
KMOD_EXPLICIT_DECL(de.theil.jitterentropy_kmod, "3.7.1",
		   jitterentropy_kmod_start, jitterentropy_kmod_stop)

__private_extern__ kmod_start_func_t *_realmain      = jitterentropy_kmod_start;
__private_extern__ kmod_stop_func_t  *_antimain      = jitterentropy_kmod_stop;
__private_extern__ int                _kext_apple_cc = __APPLE_CC__;
