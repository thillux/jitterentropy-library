/*-
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
 *
 * FreeBSD kernel module shim for the jitterentropy noise source.
 *
 * Mirrors the Linux module under linux-kmod/. The actual entropy
 * collection logic lives in src/ and is built into this module
 * unmodified; this file only adds the kernel-side glue:
 *
 *   - a misc-style character device /dev/jitterentropy; each open()
 *     allocates a private struct rand_data that backs read() and is
 *     released when the last fd is closed (via devfs_set_cdevpriv).
 *   - a character device /dev/jitterentropy-status; open() snapshots
 *     the JSON status of the shared collector and read() serves that
 *     snapshot back.
 *
 * Module tunables (set via loader.conf or sysctl -w) select the OSR
 * and feature flags applied to every collector this module allocates.
 *
 * The Linux module additionally registers a hwrng device so it feeds
 * /dev/hwrng; FreeBSD has no direct equivalent that can be wired up
 * by an out-of-tree module (random_source_register requires adding a
 * value to enum random_entropy_source, which lives in the kernel
 * tree). Userspace can feed /dev/random itself by reading from
 * /dev/jitterentropy.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/proc.h>

#include "jitterentropy.h"

MALLOC_DEFINE(M_JITTERENTROPY, "jitterentropy", "Jitter Entropy");

#define JENT_MODNAME "jitterentropy"

/* ------------------------------------------------------------------ */
/* Module tunables.                                                   */
/* ------------------------------------------------------------------ */

static unsigned int jent_osr;
static unsigned int jent_flags;
static bool         jent_ntg1;

SYSCTL_NODE(_kern, OID_AUTO, jitterentropy,
    CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "Jitter entropy noise source");

SYSCTL_UINT(_kern_jitterentropy, OID_AUTO, osr, CTLFLAG_RDTUN,
    &jent_osr, 0,
    "Oversampling rate passed to jent_entropy_collector_alloc(); "
    "0 lets the library pick its minimum.");

SYSCTL_UINT(_kern_jitterentropy, OID_AUTO, flags, CTLFLAG_RDTUN,
    &jent_flags, 0,
    "Raw flag bits passed to jent_entropy_collector_alloc(); "
    "see JENT_* in jitterentropy.h.");

SYSCTL_BOOL(_kern_jitterentropy, OID_AUTO, ntg1, CTLFLAG_RDTUN,
    &jent_ntg1, 0,
    "Enable AIS 20/31 NTG.1 mode (sets JENT_NTG1 on every collector).");

static inline unsigned int
jent_effective_flags(void)
{
	unsigned int f = jent_flags;

	if (jent_ntg1)
		f |= JENT_NTG1;
	return (f);
}

/* ------------------------------------------------------------------ */
/* Shared collector, serialized by a mutex.                           */
/* ------------------------------------------------------------------ */

static struct rand_data *jent_shared_ec;
static struct mtx jent_shared_lock;
MTX_SYSINIT(jent_shared_lock_init, &jent_shared_lock, "jent shared", MTX_DEF);

/* ------------------------------------------------------------------ */
/* /dev/jitterentropy: per-open struct rand_data via cdevpriv.        */
/* ------------------------------------------------------------------ */

static void
jent_dev_priv_dtor(void *priv)
{
	if (priv != NULL)
		jent_entropy_collector_free(priv);
}

static int
jent_dev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct rand_data *ec;
	int error;

	(void)dev;
	(void)oflags;
	(void)devtype;
	(void)td;

	ec = jent_entropy_collector_alloc(jent_osr, jent_effective_flags());
	if (ec == NULL)
		return (ENOMEM);

	error = devfs_set_cdevpriv(ec, jent_dev_priv_dtor);
	if (error != 0) {
		jent_entropy_collector_free(ec);
		return (error);
	}
	return (0);
}

static int
jent_dev_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct rand_data *ec;
	int error;

	(void)dev;
	(void)ioflag;

	error = devfs_get_cdevpriv((void **)&ec);
	if (error != 0)
		return (error);
	if (ec == NULL)
		return (ENXIO);

	while (uio->uio_resid > 0) {
		char kbuf[PAGE_SIZE];
		size_t want;
		ssize_t got;

		want = MIN((size_t)uio->uio_resid, sizeof(kbuf));
		got = jent_read_entropy_safe(&ec, kbuf, want);
		if (got < 0) {
			explicit_bzero(kbuf, sizeof(kbuf));
			return (EIO);
		}

		error = uiomove(kbuf, (int)got, uio);
		explicit_bzero(kbuf, sizeof(kbuf));
		if (error != 0)
			return (error);
	}
	return (0);
}

static struct cdevsw jent_dev_cdevsw = {
	.d_version = D_VERSION,
	.d_open    = jent_dev_open,
	.d_read    = jent_dev_read,
	.d_name    = JENT_MODNAME,
};

static struct cdev *jent_dev;

/* ------------------------------------------------------------------ */
/* /dev/jitterentropy-status: snapshot of the shared collector.       */
/* ------------------------------------------------------------------ */

struct jent_status_snap {
	size_t	len;
	char	buf[PAGE_SIZE];
};

static void
jent_status_priv_dtor(void *priv)
{
	if (priv != NULL)
		free(priv, M_JITTERENTROPY);
}

static int
jent_status_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct jent_status_snap *snap;
	int error;

	(void)dev;
	(void)oflags;
	(void)devtype;
	(void)td;

	snap = malloc(sizeof(*snap), M_JITTERENTROPY, M_WAITOK | M_ZERO);

	mtx_lock(&jent_shared_lock);
	if (jent_status(jent_shared_ec, snap->buf, sizeof(snap->buf)) < 0) {
		mtx_unlock(&jent_shared_lock);
		free(snap, M_JITTERENTROPY);
		return (EIO);
	}
	mtx_unlock(&jent_shared_lock);

	snap->len = strnlen(snap->buf, sizeof(snap->buf));
	error = devfs_set_cdevpriv(snap, jent_status_priv_dtor);
	if (error != 0) {
		free(snap, M_JITTERENTROPY);
		return (error);
	}
	return (0);
}

static int
jent_status_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct jent_status_snap *snap;
	off_t off;
	size_t left, want;
	int error;

	(void)dev;
	(void)ioflag;

	error = devfs_get_cdevpriv((void **)&snap);
	if (error != 0)
		return (error);
	if (snap == NULL)
		return (ENXIO);

	off = uio->uio_offset;
	if (off < 0 || (size_t)off >= snap->len)
		return (0);

	left = snap->len - (size_t)off;
	want = MIN((size_t)uio->uio_resid, left);

	return (uiomove(snap->buf + (size_t)off, (int)want, uio));
}

static struct cdevsw jent_status_cdevsw = {
	.d_version = D_VERSION,
	.d_open    = jent_status_open,
	.d_read    = jent_status_read,
	.d_name    = JENT_MODNAME "-status",
};

static struct cdev *jent_status_dev;

/* ------------------------------------------------------------------ */
/* Module load / unload.                                              */
/* ------------------------------------------------------------------ */

static int
jent_modload(void)
{
	unsigned int flags = jent_effective_flags();
	int ret;

	ret = jent_entropy_init_ex(jent_osr, flags);
	if (ret != 0) {
		printf(JENT_MODNAME
		    ": entropy init failed (osr=%u flags=0x%x ntg1=%d): %d\n",
		    jent_osr, flags, jent_ntg1, ret);
		return (EIO);
	}

	jent_shared_ec = jent_entropy_collector_alloc(jent_osr, flags);
	if (jent_shared_ec == NULL) {
		printf(JENT_MODNAME ": shared collector alloc failed\n");
		return (ENOMEM);
	}

	jent_dev = make_dev(&jent_dev_cdevsw, 0, UID_ROOT, GID_WHEEL, 0444,
	    JENT_MODNAME);
	jent_status_dev = make_dev(&jent_status_cdevsw, 0, UID_ROOT, GID_WHEEL,
	    0444, JENT_MODNAME "-status");

	printf(JENT_MODNAME
	    ": loaded (lib %u, osr=%u flags=0x%x ntg1=%d)\n",
	    jent_version(), jent_osr, flags, jent_ntg1);
	return (0);
}

static void
jent_modunload(void)
{
	if (jent_status_dev != NULL) {
		destroy_dev(jent_status_dev);
		jent_status_dev = NULL;
	}
	if (jent_dev != NULL) {
		destroy_dev(jent_dev);
		jent_dev = NULL;
	}

	mtx_lock(&jent_shared_lock);
	if (jent_shared_ec != NULL) {
		jent_entropy_collector_free(jent_shared_ec);
		jent_shared_ec = NULL;
	}
	mtx_unlock(&jent_shared_lock);
}

static int
jent_modevent(module_t mod, int type, void *data)
{
	(void)mod;
	(void)data;

	switch (type) {
	case MOD_LOAD:
		return (jent_modload());
	case MOD_UNLOAD:
		jent_modunload();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t jent_mod = {
	JENT_MODNAME,
	jent_modevent,
	NULL
};

DECLARE_MODULE(jitterentropy, jent_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(jitterentropy, 1);
