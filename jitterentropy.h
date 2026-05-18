/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _JITTERENTROPY_H
#define _JITTERENTROPY_H

/*
 * Set the following defines as needed for your environment
 * Compilation for AWS-LC     #define AWSLC
 * Compilation for libgcrypt  #define LIBGCRYPT
 * Compilation for OpenSSL    #define OPENSSL
 * Compilation for Linux kernel module       #define JENT_LINUX_KERNEL
 *                                          (also accepts plain #define JENT_KERNEL
 *                                           and is auto-set when __KERNEL__ is defined)
 * Compilation for FreeBSD kernel module     #define JENT_FREEBSD_KERNEL
 *                                          (auto-set when _KERNEL && __FreeBSD__)
 * Compilation for macOS (Darwin) KEXT       #define JENT_MACOS_KERNEL
 *                                          (auto-set when KERNEL && __APPLE__)
 * Compilation for bare metal / EFI / no OS  #define JENT_BAREMETAL
 *
 * JENT_KERNEL is an umbrella macro that is automatically defined whenever
 * JENT_LINUX_KERNEL or JENT_FREEBSD_KERNEL is set. Library code that does
 * not care which kernel it is running in checks JENT_KERNEL; OS-specific
 * branches in the arch/ helpers test the specific variant.
 *
 * In every JENT_KERNEL / JENT_BAREMETAL mode the user-space libc headers
 * (stdio, stdlib, unistd, time, mlock, ...) are not pulled in and the
 * arch/ helpers fall back to environment-specific implementations or
 * neutral stubs. JENT_CONF_ENABLE_INTERNAL_TIMER is also force-disabled,
 * since neither kernel exposes POSIX threads from inside a module and the
 * baremetal target has no scheduler at all.
 */

/* Linux: __KERNEL__ is set by Kbuild; promote it (and the legacy
 * plain-JENT_KERNEL spelling) to the explicit JENT_LINUX_KERNEL guard.
 * Don't promote when another kernel variant has been explicitly
 * requested. */
#if (defined(__KERNEL__) || \
     (defined(JENT_KERNEL) && !defined(JENT_FREEBSD_KERNEL) && \
      !defined(JENT_MACOS_KERNEL))) && \
    !defined(JENT_LINUX_KERNEL)
# define JENT_LINUX_KERNEL
#endif

/* FreeBSD: _KERNEL + __FreeBSD__ is the canonical detection. */
#if defined(_KERNEL) && defined(__FreeBSD__) && !defined(JENT_FREEBSD_KERNEL)
# define JENT_FREEBSD_KERNEL
#endif

/* Darwin: xnu kernel builds define KERNEL (no underscores) + __APPLE__. */
#if defined(KERNEL) && defined(__APPLE__) && !defined(JENT_MACOS_KERNEL)
# define JENT_MACOS_KERNEL
#endif

/* Umbrella macro - convenient for "any kernel" branches. */
#if (defined(JENT_LINUX_KERNEL) || defined(JENT_FREEBSD_KERNEL) || \
     defined(JENT_MACOS_KERNEL)) && \
    !defined(JENT_KERNEL)
# define JENT_KERNEL
#endif

/*
 * UINT32_C / UINT64_C are <stdint.h> macros that simply suffix a literal
 * with the right integer-promotion suffix. The Linux/FreeBSD kernel
 * headers do not pull <stdint.h>, so synthesise them where missing.
 */
#if (defined(JENT_KERNEL) || defined(JENT_BAREMETAL)) && !defined(UINT32_C)
# define UINT32_C(x) (x ## U)
#endif
#if (defined(JENT_KERNEL) || defined(JENT_BAREMETAL)) && !defined(UINT64_C)
# define UINT64_C(x) (x ## ULL)
#endif

/* <stdint.h> integer limit macros likewise missing in the kernel. */
#if defined(JENT_KERNEL) && !defined(UINT32_MAX)
# define UINT32_MAX (0xffffffffU)
#endif
#if defined(JENT_KERNEL) && !defined(UINT64_MAX)
# define UINT64_MAX (0xffffffffffffffffULL)
#endif
#if defined(JENT_KERNEL) && !defined(SIZE_MAX)
# define SIZE_MAX (~(size_t)0)
#endif

#if defined(JENT_KERNEL) || defined(JENT_BAREMETAL)
# ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
#  undef JENT_CONF_ENABLE_INTERNAL_TIMER
# endif
#endif

#if defined(JENT_LINUX_KERNEL)
# include <linux/types.h>
# include <linux/string.h>
# include <linux/errno.h>
# include <linux/limits.h>
#elif defined(JENT_FREEBSD_KERNEL)
# include <sys/types.h>
# include <sys/param.h>
# include <sys/systm.h>
# include <sys/libkern.h>
# include <sys/errno.h>
#elif defined(JENT_MACOS_KERNEL)
/*
 * Darwin / xnu kernel: the Kernel.framework headers ship POSIX-style
 * types under <sys/...> and <mach/...>. libkern.h supplies printf,
 * snprintf, strlen, memcpy, memset, bzero with the standard signatures.
 */
# include <sys/types.h>
# include <sys/errno.h>
# include <sys/systm.h>
# include <libkern/libkern.h>
#elif defined(JENT_BAREMETAL)
# include <stddef.h>
# include <stdint.h>
# ifndef SSIZE_MAX
#  define SSIZE_MAX ((ssize_t)((~(size_t)0) >> 1))
# endif
/*
 * Baremetal: provide a minimal ssize_t and the small set of errno values
 * the library returns through its API. The consumer is expected not to
 * depend on a full libc <errno.h>.
 */
# ifndef _JENT_BAREMETAL_TYPES
#  define _JENT_BAREMETAL_TYPES
typedef long ssize_t;
# endif
# ifndef EINVAL
#  define EINVAL 22
# endif
# ifndef ENOMEM
#  define ENOMEM 12
# endif
# ifndef EAGAIN
#  define EAGAIN 11
# endif
# ifndef EFAULT
#  define EFAULT 14
# endif
# ifndef EBUSY
#  define EBUSY 16
# endif
# ifndef ENOENT
#  define ENOENT 2
# endif
# ifndef EIO
#  define EIO 5
# endif
# ifndef ETIMEDOUT
#  define ETIMEDOUT 110
# endif
# ifndef EPERM
#  define EPERM 1
# endif
# ifndef EINTR
#  define EINTR 4
# endif
/*
 * Baremetal callers must supply memcpy/memset (typically via a freestanding
 * compiler-rt or libgcc). They are declared here for the headers that need
 * them; no <string.h> include is performed.
 */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
#else /* hosted user-space build */
# include <limits.h>
# include <time.h>
# include <stdint.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <errno.h>

# if defined(_MSC_VER) || defined(__MINGW32__)
#  include <windows.h>
typedef int64_t ssize_t;
# else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
# endif

# ifdef __MACH__
#  include <assert.h>
#  include <CoreServices/CoreServices.h>
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#  include <unistd.h>
# endif
#endif /* JENT_KERNEL / JENT_BAREMETAL */

/*
 * Architecture- and OS-specific helpers (timestamp, secure memory, cache
 * size discovery, online CPU count, FIPS mode detection, scheduler yield)
 * live in dedicated shared headers that internally select the right
 * implementation via #ifdefs.
 */
#include "arch/jitterentropy-arch-timer.h"
#include "arch/jitterentropy-arch-memory.h"
#include "arch/jitterentropy-arch-cache.h"
#include "arch/jitterentropy-arch-ncpu.h"
#include "arch/jitterentropy-arch-fips.h"
#include "arch/jitterentropy-arch-sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * API / ABI incompatible changes, functional changes that require consumer to
 * be updated (as long as this number is zero, the API is not considered stable
 * and can change without a bump of the major version).
 */
#define JENT_MAJVERSION 3

/*
 * API compatible, ABI may change, functional enhancements only, consumer can be
 * left unchanged if enhancements are not considered.
 */
#define JENT_MINVERSION 7

/*
 * API / ABI compatible, no functional changes, no enhancements, bug fixes only.
 * Also, the entropy collection is not changed in any way that would necessitate
 * a re-assessment.
 */
#define JENT_PATCHLEVEL 1

#define JENT_VERSION (JENT_MAJVERSION * 1000000 + \
		      JENT_MINVERSION * 10000 + \
		      JENT_PATCHLEVEL * 100)

/* -- BEGIN Main interface functions -- */
/* Flags that can be used to initialize the RNG */
#define JENT_DISABLE_STIR (1<<0) 	/* UNUSED */
#define JENT_DISABLE_UNBIAS (1<<1) 	/* UNUSED */
#define JENT_DISABLE_MEMORY_ACCESS (1<<2) /* Disable memory access for more
					     entropy, saves MEMORY_SIZE RAM for
					     entropy collector */
#define JENT_FORCE_INTERNAL_TIMER (1<<3)  /* Force the use of the internal
					     timer */
#define JENT_DISABLE_INTERNAL_TIMER (1<<4)  /* Disable the potential use of
					       the internal timer. */
#define JENT_FORCE_FIPS (1<<5)		  /* Force FIPS compliant mode
					     including full SP800-90B
					     compliance. */
#define JENT_NTG1 (1<<6) /* AIS 20/31 NTG.1 compliance */
#define JENT_CACHE_ALL (1<<7) /* Shall size of all caches be used to
				 automatically determine the memory size for the
				 memory access? By default it is only the L1
				 cache size. */

/* Flags field limiting the amount of memory to be used for memory access */
#define JENT_FLAGS_TO_MEMSIZE_SHIFT	27
#define JENT_FLAGS_TO_MAX_MEMSIZE(val)	((val) >> JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_TO_FLAGS(val)	((val) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_1kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 1))
#define JENT_MAX_MEMSIZE_2kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 2))
#define JENT_MAX_MEMSIZE_4kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 3))
#define JENT_MAX_MEMSIZE_8kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 4))
#define JENT_MAX_MEMSIZE_16kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 5))
#define JENT_MAX_MEMSIZE_32kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 6))
#define JENT_MAX_MEMSIZE_64kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 7))
#define JENT_MAX_MEMSIZE_128kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 8))
#define JENT_MAX_MEMSIZE_256kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 9))
#define JENT_MAX_MEMSIZE_512kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(10))
#define JENT_MAX_MEMSIZE_1MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(11))
#define JENT_MAX_MEMSIZE_2MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(12))
#define JENT_MAX_MEMSIZE_4MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(13))
#define JENT_MAX_MEMSIZE_8MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(14))
#define JENT_MAX_MEMSIZE_16MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(15))
#define JENT_MAX_MEMSIZE_32MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(16))
#define JENT_MAX_MEMSIZE_64MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(17))
#define JENT_MAX_MEMSIZE_128MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(18))
#define JENT_MAX_MEMSIZE_256MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(19))
#define JENT_MAX_MEMSIZE_512MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(20))
#define JENT_MAX_MEMSIZE_MAX		JENT_MAX_MEMSIZE_512MB
#define JENT_MAX_MEMSIZE_MASK		JENT_MAX_MEMSIZE_TO_FLAGS(0xffffffff)
/*
 * We start at 1kB -> offset is log2(1024) - 1 as the flag value above is added
 * to this offset.
 */
#define JENT_MAX_MEMSIZE_OFFSET		9

/* Flags field defining the hash loop */
#define JENT_FLAGS_TO_HASHLOOP_SHIFT	24
#define JENT_HASHLOOP_TO_FLAGS(val)	((val) << JENT_FLAGS_TO_HASHLOOP_SHIFT)
#define JENT_MAX_HASHLOOP_MASK		JENT_HASHLOOP_TO_FLAGS(0x7)
#define JENT_FLAGS_TO_HASHLOOP(val)	(((val) >> JENT_FLAGS_TO_HASHLOOP_SHIFT)\
					 & 0x7)
#define JENT_HASHLOOP_1			JENT_HASHLOOP_TO_FLAGS(UINT32_C(0))
#define JENT_HASHLOOP_2			JENT_HASHLOOP_TO_FLAGS(UINT32_C(1))
#define JENT_HASHLOOP_4			JENT_HASHLOOP_TO_FLAGS(UINT32_C(2))
#define JENT_HASHLOOP_8			JENT_HASHLOOP_TO_FLAGS(UINT32_C(3))
#define JENT_HASHLOOP_16		JENT_HASHLOOP_TO_FLAGS(UINT32_C(4))
#define JENT_HASHLOOP_32		JENT_HASHLOOP_TO_FLAGS(UINT32_C(5))
#define JENT_HASHLOOP_64		JENT_HASHLOOP_TO_FLAGS(UINT32_C(6))
#define JENT_HASHLOOP_128		JENT_HASHLOOP_TO_FLAGS(UINT32_C(7))
#define JENT_MAX_HASHLOOP		JENT_HASHLOOP_128

#ifdef JENT_PRIVATE_COMPILE
# define JENT_PRIVATE_STATIC static
#else /* JENT_PRIVATE_COMPILE */
#if defined(_MSC_VER)
#define JENT_PRIVATE_STATIC __declspec(dllexport)
#else
#define JENT_PRIVATE_STATIC __attribute__((visibility("default")))
#endif
#endif

#if !defined(JENT_KERNEL) && !defined(JENT_BAREMETAL) && \
    (defined(__MINGW32__) || defined(__APPLE__) || defined(__OpenBSD__) || \
     defined(__FreeBSD__) || defined(__NetBSD__))
#define JENT_PTHREAD
#endif

/* Forward declaration of opaque value */
struct rand_data;

/* Number of low bits of the time value that we want to consider */
/* get raw entropy */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t len);
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy_safe(struct rand_data **ec, char *data, size_t len);
/* initialize an instance of the entropy collector */
JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
	       				       unsigned int flags);
/* clearing of entropy collector */
JENT_PRIVATE_STATIC
void jent_entropy_collector_free(struct rand_data *entropy_collector);

/* initialization of entropy collector */
JENT_PRIVATE_STATIC
int jent_entropy_init(void);
JENT_PRIVATE_STATIC
int jent_entropy_init_ex(unsigned int osr, unsigned int flags);

/*
 * Set a callback to run on health failure in FIPS mode.
 * This function will take an action determined by the caller.
 */
typedef void (*jent_fips_failure_cb)(struct rand_data *ec,
				     unsigned int health_failure);
JENT_PRIVATE_STATIC
int jent_set_fips_failure_callback(jent_fips_failure_cb cb);

/* return version number of core library */
JENT_PRIVATE_STATIC
unsigned int jent_version(void);

/* print out human-readable status of the Jitter RNG (JSON) */
JENT_PRIVATE_STATIC
int jent_status(const struct rand_data *ec, char *buf, size_t buflen);

/* return secure memory support, must be done
 * in jitterentropy itself, as users may not define
 * a crypto library and so the define in arch/jitterentropy-arch-memory.h
 * is not set for them. */
JENT_PRIVATE_STATIC
int jent_secure_memory_supported(void);

/**
 * Function pointer data structure to register an external thread handler
 * used for the timer-less mode of the Jitter RNG.
 *
 * The external caller provides these function pointers to handle the
 * management of the timer thread that is spawned by the Jitter RNG.
 *
 * @var jent_notime_init This function is intended to initialize the threading
 *	support. All data that is required by the threading code must be
 *	held in the data structure ctx. The Jitter RNG maintains the
 *	data structure and uses it for every invocation of the following calls.
 *
 * @var jent_notime_fini This function shall terminate the threading support.
 *	The function must dispose of all memory and resources used for the
 *	threading operation. It must also dispose of the ctx memory.
 *
 * @var jent_notime_start This function is called when the Jitter RNG wants
 *	to start a thread. Besides providing a pointer to the ctx
 *	allocated during initialization time, the Jitter RNG provides a
 *	pointer to the function the thread shall execute and the argument
 *	the function shall be invoked with. These two parameters have the
 *	same purpose as the trailing two parameters of pthread_create(3).
 *
 * @var jent_notime_stop This function is invoked by the Jitter RNG when the
 *	thread should be stopped. Note, the Jitter RNG intends to start/stop
 *	the thread frequently.
 *
 * An example implementation is found in the Jitter RNG itself with its
 * default thread handler of jent_notime_thread_builtin.
 *
 * If the caller wants to register its own thread handler, it must be done
 * with the API call jent_entropy_switch_notime_impl as the first
 * call to interact with the Jitter RNG, even before jent_entropy_init.
 * After jent_entropy_init is called, changing of the threading implementation
 * is not allowed.
 */
struct jent_notime_thread {
	int (*jent_notime_init)(void **ctx);
	void (*jent_notime_fini)(void *ctx);
	int (*jent_notime_start)(void *ctx,
#ifdef JENT_PTHREAD
		void *(*start_routine) (void *), void *arg);
#else
		int (*start_routine)(void *), void *arg);
#endif
	void (*jent_notime_stop)(void *ctx);
};

/* Set a different thread handling logic for the notimer support */
JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread);
/* -- END of Main interface functions -- */

/* -- BEGIN timer-less threading support functions to prevent code dupes -- */
JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx);

JENT_PRIVATE_STATIC
void jent_notime_fini(void *ctx);
/* -- END timer-less threading support functions to prevent code dupes -- */

/* -- BEGIN error codes for init function -- */
#define ENOTIME  	1 /* Timer service not available */
#define ECOARSETIME	2 /* Timer too coarse for RNG */
#define ENOMONOTONIC	3 /* Timer is not monotonic increasing */
#define EMINVARIATION	4 /* Timer variations too small for RNG */
#define EVARVAR		5 /* Timer does not produce variations of variations
			     (2nd derivation of time is zero) */
#define EMINVARVAR	6 /* Timer variations of variations is too small */
#define EPROGERR	7 /* Programming error */
#define ESTUCK		8 /* Too many stuck results during init. */
#define EHEALTH		9 /* Health test failed during initialization */
#define ERCT		10 /* RCT failed during initialization */
#define EHASH		11 /* Hash self test failed */
#define EMEM		12 /* Can't allocate memory for initialization */
#define EGCD		13 /* GCD self-test failed */
/* -- END error codes for init function -- */

/* -- BEGIN error masks for health tests -- */
#define JENT_RCT_FAILURE	1 /* Failure in RCT health test. */
#define JENT_APT_FAILURE	2 /* Failure in APT health test. */
#define JENT_LAG_FAILURE	4 /* Failure in Lag predictor health test. */
#define JENT_RCT_MEM_FAILURE	8 /* Failure in RCT with memory health test. */
#define JENT_PERMANENT_FAILURE_SHIFT	16
#define JENT_PERMANENT_FAILURE(x)	((x) << JENT_PERMANENT_FAILURE_SHIFT)
#define JENT_RCT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_RCT_FAILURE)
#define JENT_APT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_APT_FAILURE)
#define JENT_LAG_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_LAG_FAILURE)
#define JENT_RCT_MEM_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_RCT_MEM_FAILURE)
/* -- END error masks for health tests -- */

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_H */
