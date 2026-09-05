/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel module handling for Jitter RNG.
 *
 * Defines the module parameters shared by the kernel interfaces and ties the
 * interfaces (crypto API, hwrng, character device, procfs status, test
 * interface) together in the module init/exit paths.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

/*
 * MODULE_ALIAS_CRYPTO() lives in crypto/algapi.h on kernels >= 6.4 and in
 * linux/crypto.h before; crypto/algapi.h includes linux/crypto.h, so this
 * single include covers the whole supported kernel range. Only needed for
 * the crypto API alias emitted at the bottom of this file.
 */
#ifdef CONFIG_EXTERNAL_JITTERENTROPY_KCAPI
#include <crypto/algapi.h>
#endif
#include <linux/fips.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_compat.h"
#include "jitterentropy_hwrng.h"
#include "jitterentropy_kcapi.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_testing.h"

/*
 * Kernel module options.
 *
 * osr, flags and verbose are non-static as they are shared with the kernel
 * interfaces: osr and flags with the crypto API, hwrng and character device
 * interfaces, verbose with the crypto API and test interfaces (see
 * jitterentropy_kcapi.c and jitterentropy_testing.c).
 */
unsigned int osr = 0;
unsigned int flags = 0;
unsigned int verbose = 0;

/*
 * Shortcut parameters for common operation modes. They are folded into the
 * shared flags value in jent_mod_init(), so the effective configuration is
 * visible in the flags sysfs file; the numeric flag bits do not need to be
 * known to request the modes.
 */
static bool ntg1 = false;
static bool force_fips = false;
static bool cache_all = false;

/*
 * Size of the memory access region of every instance, in kB: a power of two
 * up to 524288 (512 MB), 0 (the default) leaving it to the library.
 *
 * Two things the size otherwise grows through, neither of which
 * max_instances bounds - that caps the number of instances, not the memory
 * one of them ends up with. The library derives the size from the CPU cache
 * size, so cache_all alone takes it into the hundreds of MB. And every
 * reallocation on health-test recovery doubles it, which takes an instance to
 * the 512 MB ceiling from any starting size in a dozen recoveries, cache_all
 * or not; that path exists in a compliance mode only (fips=1, force_fips or
 * ntg1), the only one whose health tests report a failure to recover from.
 *
 * Setting this pins the size against both: the cache-size derivation is
 * skipped and the recovery keeps the size it finds, raising only the
 * oversampling rate and the hash loop count. It takes precedence over a size
 * given through the flags parameter.
 */
static unsigned int max_memsize;

module_param(osr, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(osr, "Jitter RNG OSR parameter");
module_param(flags, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(flags, "Jitter RNG flags parameter");
module_param(verbose, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(verbose, "Jitter RNG verbose logging");
module_param(ntg1, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(ntg1, "Enable AIS 20/31 NTG.1 compliant operation (shortcut for the JENT_NTG1 bit in flags)");
module_param(force_fips, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(force_fips, "Force FIPS compliant operation (shortcut for the JENT_FORCE_FIPS bit in flags)");
module_param(cache_all, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(cache_all, "Derive the memory access region from the size of all caches instead of L1 only (shortcut for the JENT_CACHE_ALL bit in flags)");
module_param(max_memsize, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_memsize,
		 "Memory access region of an instance in kB, a power of two up to 524288 (shortcut for the JENT_MAX_MEMSIZE_* bits in flags; 0: derived from the cache size and grown by health-test recovery)");

static int __init jent_mod_init(void)
{
	int ret = 0;

	/*
	 * Fold the shortcut parameters into the shared flags value before any
	 * user of it runs: the interfaces (crypto API, hwrng, character
	 * device) allocate their collectors from flags at open/instantiation
	 * time, and the sysfs flags file then reports the effective value.
	 */
	if (ntg1)
		flags |= JENT_NTG1;
	if (force_fips)
		flags |= JENT_FORCE_FIPS;
	if (cache_all)
		flags |= JENT_CACHE_ALL;

	if (max_memsize) {
		/*
		 * The field encodes 1 kB << (field - 1), so the size has to be
		 * a power of two the field can hold. One that is not is a
		 * configuration error and refuses the load: rounding it would
		 * hand out a memory region of a size nobody asked for, and
		 * this parameter exists to bound exactly that.
		 */
		unsigned int max_kb = 1U <<
			(JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX) - 1);

		if (!is_power_of_2(max_memsize) || max_memsize > max_kb) {
			pr_err("jitterentropy: max_memsize %u kB is not a power of two of at most %u kB\n",
			       max_memsize, max_kb);
			return -EINVAL;
		}

		flags &= ~(unsigned int)JENT_MAX_MEMSIZE_MASK;
		flags |= JENT_MAX_MEMSIZE_TO_FLAGS(ilog2(max_memsize) + 1);
	}

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		/* Handle permanent health test error */
		if (fips_enabled)
			panic("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);

		pr_info("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);
		return -EFAULT;
	}

	/*
	 * Validate the interval the interfaces' instances schedule their own
	 * periodic self test runs with, before any of them is registered. The
	 * known answer tests themselves have just run inside
	 * jent_entropy_init_ex() above, which refused the load on failure.
	 */
	jent_selftest_init();

	ret = jent_proc_init();
	if (ret)
		return ret;

	ret = jent_kcapi_init();
	if (ret)
		goto err;

	ret = jent_hwrng_init();
	if (ret)
		goto err_crypto;

	/*
	 * Of the two userspace-visible interfaces, the debugfs one is the
	 * safer to unwind: its proxy fails all file operations after
	 * debugfs_remove_recursive(), and nothing opens it automatically the
	 * way udev opens /dev/jitterentropy. So it goes first and the
	 * character device below is the last fallible step.
	 */
	ret = jent_testing_init();
	if (ret)
		goto err_hwrng;

	/*
	 * The very last step: misc_register() makes /dev/jitterentropy
	 * openable immediately and misc_deregister() does not wait for open
	 * files, so a later init failure could free the module under an
	 * already-open file. The hwrng may precede it - hwrng_unregister()
	 * drains its readers.
	 */
	ret = jent_chardev_init();
	if (ret)
		goto err_testing;

	return 0;

err_testing:
	jent_testing_exit();
err_hwrng:
	jent_hwrng_exit();
err_crypto:
	jent_kcapi_exit();
err:
	jent_proc_exit();
	return ret;
}

static void __exit jent_mod_exit(void)
{
	/*
	 * Reverse order of the registrations in jent_mod_init(). The character
	 * device and the hwrng must precede jent_proc_exit(): they remove
	 * status files below /proc/jitterentropy, which it removes
	 * recursively, and a proc_remove() on an already-removed entry would
	 * act on freed memory. Each interface stops the self tests of its own
	 * instances as it tears them down.
	 */
	jent_chardev_exit();
	jent_testing_exit();
	jent_hwrng_exit();
	jent_kcapi_exit();
	jent_proc_exit();
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Non-physical True Random Number Generator based on CPU Jitter");
/*
 * The crypto API name depends on the build mode (see jent_alg.base.cra_name
 * in jitterentropy_kcapi.c). Alias the matching name so the algorithm can be
 * auto-loaded on request via the kernel crypto API. Only emitted when the
 * crypto API interface is compiled in: the alias must not advertise an
 * algorithm this build does not register.
 */
#ifdef CONFIG_EXTERNAL_JITTERENTROPY_KCAPI
# ifdef CONFIG_BUILTIN_JITTERENTROPY
MODULE_ALIAS_CRYPTO("jitterentropy_rng");
# else
MODULE_ALIAS_CRYPTO("jitter_rng");
# endif
#endif /* CONFIG_EXTERNAL_JITTERENTROPY_KCAPI */
