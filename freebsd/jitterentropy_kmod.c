/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * FreeBSD kernel module wrapping the jitterentropy library. Exposes a
 * /dev/jitterentropy character device returning random bytes from the
 * jitter RNG, with osr and flags exposed as kenv-tunable / sysctl-
 * settable module parameters.
 *
 * The jitterentropy core sources are compiled in with -DJENT_KERNEL,
 * which together with -D_KERNEL + __FreeBSD__ selects the
 * JENT_FREEBSD_KERNEL arch branches (kernel malloc, mp_ncpus, ...).
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

/* --- tunables ----------------------------------------------------------- */

static unsigned int jent_osr = 0;
static unsigned int jent_flags = 0;

SYSCTL_NODE(_kern, OID_AUTO, jitterentropy,
	    CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
	    "Jitter RNG (jitterentropy) module parameters");

SYSCTL_UINT(_kern_jitterentropy, OID_AUTO, osr,
	    CTLFLAG_RDTUN, &jent_osr, 0,
	    "Oversampling rate (0 = library default JENT_MIN_OSR)");

SYSCTL_UINT(_kern_jitterentropy, OID_AUTO, flags,
	    CTLFLAG_RDTUN, &jent_flags, 0,
	    "Bitwise OR of JENT_* flags (see jitterentropy.h)");

/* Both tunables are read at module load via kenv:
 *   kenv kern.jitterentropy.osr=8
 *   kenv kern.jitterentropy.flags=0x20
 * before kldload jitterentropy_kmod. */

/* --- module state ------------------------------------------------------- */

static struct mtx jent_lock;
static struct rand_data *jent_ec;
static struct cdev *jent_cdev;

#define JENT_CHRDEV_CHUNK 256

/*
 * Flags that don't make sense in the kernel build: the internal timer is
 * compiled out, so both forcing and disabling it are rejected.
 */
#define JENT_KMOD_REJECT_FLAGS \
	(JENT_FORCE_INTERNAL_TIMER | JENT_DISABLE_INTERNAL_TIMER)

/* --- character device --------------------------------------------------- */

static int
jent_chrdev_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	unsigned char buf[JENT_CHRDEV_CHUNK];
	int error = 0;

	(void)dev;
	(void)ioflag;

	while (uio->uio_resid > 0) {
		size_t want = MIN((size_t)uio->uio_resid, sizeof(buf));
		ssize_t got;

		mtx_lock(&jent_lock);
		got = jent_read_entropy_safe(&jent_ec, (char *)buf, want);
		mtx_unlock(&jent_lock);

		if (got <= 0) {
			explicit_bzero(buf, sizeof(buf));
			return (EIO);
		}

		error = uiomove(buf, got, uio);
		explicit_bzero(buf, sizeof(buf));
		if (error)
			return (error);
	}

	return (0);
}

static struct cdevsw jent_cdevsw = {
	.d_version = D_VERSION,
	.d_read    = jent_chrdev_read,
	.d_name    = "jitterentropy",
};

/* --- module event handler ----------------------------------------------- */

static int
jent_modevent(module_t mod, int what, void *arg)
{
	int ret;

	(void)mod;
	(void)arg;

	switch (what) {
	case MOD_LOAD:
		if (jent_flags & JENT_KMOD_REJECT_FLAGS) {
			printf("jitterentropy_kmod: JENT_FORCE_INTERNAL_TIMER "
			    "and JENT_DISABLE_INTERNAL_TIMER are not "
			    "meaningful in kernel mode (flags=0x%x)\n",
			    jent_flags);
			return (EINVAL);
		}

		ret = jent_entropy_init_ex(jent_osr, jent_flags);
		if (ret) {
			printf("jitterentropy_kmod: jent_entropy_init_ex"
			    "(osr=%u, flags=0x%x) failed: %d\n",
			    jent_osr, jent_flags, ret);
			return (EIO);
		}

		mtx_init(&jent_lock, "jitterentropy", NULL, MTX_DEF);

		jent_ec = jent_entropy_collector_alloc(jent_osr, jent_flags);
		if (jent_ec == NULL) {
			mtx_destroy(&jent_lock);
			printf("jitterentropy_kmod: "
			    "jent_entropy_collector_alloc returned NULL\n");
			return (ENOMEM);
		}

		jent_cdev = make_dev(&jent_cdevsw, 0, UID_ROOT, GID_WHEEL,
		                     0444, "jitterentropy");
		if (jent_cdev == NULL) {
			jent_entropy_collector_free(jent_ec);
			jent_ec = NULL;
			mtx_destroy(&jent_lock);
			printf("jitterentropy_kmod: make_dev failed\n");
			return (ENXIO);
		}

		printf("jitterentropy_kmod: registered /dev/jitterentropy "
		    "(osr=%u, flags=0x%x, lib v%u)\n",
		    jent_osr, jent_flags, jent_version());
		return (0);

	case MOD_UNLOAD:
		if (jent_cdev != NULL) {
			destroy_dev(jent_cdev);
			jent_cdev = NULL;
		}
		mtx_lock(&jent_lock);
		jent_entropy_collector_free(jent_ec);
		jent_ec = NULL;
		mtx_unlock(&jent_lock);
		mtx_destroy(&jent_lock);
		printf("jitterentropy_kmod: unloaded\n");
		return (0);

	case MOD_QUIESCE:
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t jent_mod = {
	"jitterentropy_kmod",
	jent_modevent,
	NULL
};

DECLARE_MODULE(jitterentropy_kmod, jent_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(jitterentropy_kmod, 1);
