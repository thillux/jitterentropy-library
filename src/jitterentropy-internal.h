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

#ifndef _JITTERENTROPY_INTERNAL_H
#define _JITTERENTROPY_INTERNAL_H

#include "jitterentropy.h"

/*
 * Architecture- and OS-specific helpers (timestamp, secure memory, cache
 * size discovery, online CPU count, FIPS mode detection, scheduler yield,
 * atomic access to the process-wide latches) live in dedicated shared headers.
 * The ones that can be expressed inline select the right implementation
 * through #ifdefs here; the rest declare what the matching source file under
 * arch/ defines, which is where a platform's own headers stay confined.
 */
#include "arch/jitterentropy-arch-atomic.h"
#include "arch/jitterentropy-arch-timer.h"
#include "arch/jitterentropy-arch-memory.h"
#include "arch/jitterentropy-arch-cache.h"
#include "arch/jitterentropy-arch-ncpu.h"
#include "arch/jitterentropy-arch-fips.h"
#include "arch/jitterentropy-arch-sched.h"
#include "arch/jitterentropy-arch-random.h"
#include "jitterentropy-uuid.h"

/*
 * The entropy core must be compiled without optimization - the #error on
 * __OPTIMIZE__ in jitterentropy-base.c is what refuses anything else. That
 * macro is GCC's and Clang's; MSVC defines no macro that says whether it is
 * optimizing, so the check cannot fire there, and cl /O2 compiled the noise
 * source clean. The shipped MSVC build was unoptimized only because
 * CMakeLists.txt appended /Od after the /O2 of the configuration flags and cl
 * takes the last one - a property of one build system's flag order, which any
 * other build of these sources would not have.
 *
 * Here the property is made intrinsic to the source as far as it can be. The
 * pragma turns off the optimizations - the g, s, t and y of /O - for every
 * function defined after it, in every translation unit that includes this
 * header: wider than the noise source, but MSVC builds are hosted and built
 * /Od throughout, so nothing is lost.
 *
 * What it does not turn off is inline expansion. /Ob is none of g, s, t or y,
 * and measured on MSVC 14.44 a build at /O2 with this pragma alone keeps
 * every loop and every function but still folds the small helpers into their
 * callers - the time stamp read, jent_delta(), jent_udiv64(), the rotate of
 * the memory access PRNG - so that the instruction stream the timing runs
 * over is not the /Od one. JENT_NOINLINE below marks those helpers, which
 * makes the measured path stand on its own; /Ob0 on the command line, which
 * CMakeLists.txt supplies, is what makes the rest of the build the /Od one
 * (the memcpy intrinsic aside). A build system of its own has to add /Ob0
 * itself: the pragma plus /Ob0 is the MSVC spelling of -O0, and neither half
 * is it alone.
 *
 * clang-cl is left out: it defines __OPTIMIZE__ as Clang does and takes the
 * #error path, and it ignores this pragma with a warning.
 *
 * Nor does the pragma reach /GL. Under it the compiler emits no machine code
 * at all and code generation is deferred to the linker (/LTCG), where whether
 * the pragma holds is not something the compile step can witness.
 * CMakeLists.txt refuses /GL, /LTCG and CMAKE_INTERPROCEDURAL_OPTIMIZATION on
 * MSVC for that reason; a build system of its own has to do the same.
 */
#if defined(_MSC_VER) && !defined(__clang__)
# pragma optimize("", off)
#endif

/*
 * The helpers on the measured path that MSVC would otherwise expand inline
 * whatever the pragma says - see above. MSVC only: GCC and Clang expand
 * nothing at -O0, which the #error in jitterentropy-base.c holds them to, and
 * the kernel spells its own inline attributes, so an attribute added here
 * would only be something to conflict with there.
 */
#if defined(_MSC_VER) && !defined(__clang__)
# define JENT_NOINLINE	__declspec(noinline)
#else
# define JENT_NOINLINE
#endif

#ifdef LINUX_KERNEL
/*
 * Kernel div64 primitives backing jent_udiv64()/jent_umod64() below. The
 * header is deliberately lightweight (types, math, the arch div64
 * primitives), so it is safe for the -O0 entropy-collection core including
 * this file.
 */
#include <linux/math64.h>	/* div64_u64(), div64_u64_rem() */
/*
 * memcpy()/memset() are used by the core itself (jitterentropy-base.c,
 * jitterentropy-noise.c, jitterentropy-sha3.c). They used to arrive here by
 * accident, dragged in behind <linux/timex.h> via the arch timer header; that
 * header now lives in arch/jitterentropy-arch-timer.c and the core no longer
 * sees it, so the dependency is stated where it is actually used. <linux/
 * string.h> is itself fine at -O0 - it was already being compiled that way
 * through the transitive path this replaces.
 */
#include <linux/string.h>	/* memcpy(), memset() */
/*
 * Reached the core the same accidental way: -EAGAIN is returned by
 * jitterentropy-gcd.c and jitterentropy-health.c, and the kernel's
 * fallthrough attribute backs JENT_FALLTHROUGH below for kernel builds. Both
 * used to arrive behind <linux/timex.h> -> <linux/kernel.h>. Neither header is
 * heavier than the ones above and both are safe for the -O0 core.
 */
#include <linux/errno.h>	/* EAGAIN */
#include <linux/compiler.h>	/* fallthrough */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LINUX_KERNEL

#ifndef UINT32_MAX
# define UINT32_MAX	(4294967295U)
#endif
#ifndef UINT64_C
/* #ifdef, not #if: kernel builds run with -Wundef and 32-bit targets do not
 * define __LP64__ at all. */
# ifdef __LP64__
#  define UINT64_C(c)   c ## UL
# else
#  define UINT64_C(c)   c ## ULL
# endif
#endif

/* The kernel supplies a fallthrough macro of its own. */
#define JENT_FALLTHROUGH	fallthrough

/*
 * Test interface support (see jitterentropy-base.c): allocate an entropy
 * collector without running the startup entropy collection and its
 * health-test reset ladder. Only intended for the kernel test interface
 * (linux_kernel/jitterentropy_testing.c).
 */
struct rand_data *jent_entropy_collector_alloc_raw(unsigned int osr,
						   unsigned int flags);

/*
 * 64-bit division / modulo with a 64-bit divisor.
 *
 * The plain C operators on 64-bit operands are lowered to libgcc helper
 * calls (__udivdi3, __aeabi_uldivmod, ...) on 32-bit kernels, and the kernel
 * does not provide those helpers; route the operations through the kernel's
 * div64 primitives instead. On 64-bit kernels both primitives are inline
 * plain divisions, so code generation there is identical to the operators.
 */
static inline JENT_NOINLINE
uint64_t jent_udiv64(uint64_t dividend, uint64_t divisor)
{
	return div64_u64(dividend, divisor);
}

static inline JENT_NOINLINE
uint64_t jent_umod64(uint64_t dividend, uint64_t divisor)
{
	uint64_t rem;

	div64_u64_rem(dividend, divisor, &rem);
	return rem;
}

#else /* LINUX_KERNEL */

/*
 * Deliberately JENT_-prefixed rather than the bare lowercase "fallthrough".
 * This header is included before the platform headers in several translation
 * units, and a macro by that name silently rewrites any system header that
 * probes for the attribute - Apple's <os/base.h>, reached through
 * <mach/mach.h>, does exactly that with __has_attribute(fallthrough) and
 * fails to compile once the bare macro is in scope.
 *
 * __has_attribute() itself must be probed with defined() first: compilers
 * that do not provide it (MSVC) replace the unknown identifier with 0 and
 * then choke on the leftover "(__fallthrough__)" argument list - a constraint
 * violation, which MSVC reports as warning C4067 in every translation unit
 * including this header.
 */
#if defined(__has_attribute)
# if __has_attribute(__fallthrough__)
#  define JENT_FALLTHROUGH	__attribute__((__fallthrough__))
# endif
#endif
#ifndef JENT_FALLTHROUGH
# define JENT_FALLTHROUGH	do {} while (0)
#endif

/*
 * 64-bit division / modulo with a 64-bit divisor; see the kernel branch
 * above for the rationale. Userspace links against libgcc (or an
 * equivalent), so the plain operators are used directly.
 */
static inline JENT_NOINLINE
uint64_t jent_udiv64(uint64_t dividend, uint64_t divisor)
{
	return dividend / divisor;
}

static inline JENT_NOINLINE
uint64_t jent_umod64(uint64_t dividend, uint64_t divisor)
{
	return dividend % divisor;
}

#endif /* LINUX_KERNEL */

/*
 * An instance that measures a clock rather than generating entropy from it:
 * the startup's own collector, and the raw noise recording. Both want the
 * deltas as the clock produces them, and the first of them is what
 * establishes the common divisor the others are normalized by - so these are
 * the only instances allowed to run without one.
 *
 * Internal, and not in jitterentropy.h: the library sets it on the flags it
 * passes down, a caller never does. The public allocation clears it. The bit
 * is above the public flags and below the hash loop field; internal flags
 * grow downwards from here.
 */
#define JENT_INT_MEASURE_CLOCK	(UINT32_C(1) << 23)

/*
 * The memory size in the flags is the caller's choice, not the library's
 * derivation. Set by jent_entropy_collector_alloc() when the caller passed a
 * JENT_MAX_MEMSIZE_* value, and by jent_health_failure_reset() when the
 * collector being replaced records one; jent_entropy_collector_alloc_internal()
 * reads it into ->max_mem_set and does not store it.
 *
 * The size field itself cannot say this. Every collector's flags carry one
 * once jent_update_memsize() has normalized them, so a reallocation handed
 * those flags looked caller-pinned whatever the caller had done, and the
 * startup ladder it then ran raised the oversampling rate and the hash loop
 * count while leaving the memory region where it was.
 */
#define JENT_INT_MEMSIZE_PINNED	(UINT32_C(1) << 22)

/*
 * JENT_-prefixed, and defined outside the LINUX_KERNEL split above, for the
 * same reason JENT_FALLTHROUGH is: the bare names belong to the environment,
 * not to this library.
 *
 * jitterentropy.h deliberately does not pull in <linux/module.h> (and so not
 * <linux/kernel.h>), which keeps the -O0 entropy core free of headers that do
 * not compile without optimisation - but it also means the kernel's
 * ARRAY_SIZE()/BUILD_BUG_ON() are not available to the core, so equivalents
 * have to be defined here.
 *
 * Spelling them with the kernel's names and an #ifndef guard is what this did
 * before, and it only worked by accident: some other header had to define them
 * first for the guard to suppress ours. That held while <linux/timex.h> pulled
 * <linux/kernel.h> into every translation unit; once the timer backend moved
 * out (see arch/jitterentropy-arch-timer.c) our definitions landed first
 * instead, and every file that later included a kernel header got a
 * "'ARRAY_SIZE' redefined" warning. A distinct name cannot collide in either
 * order, on any kernel, so no guard is needed.
 *
 * linux_kernel/ is deliberately not converted: that layer includes
 * <linux/kernel.h> itself and uses the kernel's own macros, as kernel code
 * should.
 */
#define JENT_BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#define JENT_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#ifndef JENT_STUCK_INIT_THRES
/*
 * Per default, not more than 90% of all measurements during initialization
 * are allowed to be stuck.
 *
 * It is allowed to change this value as required for the intended environment.
 */
#define JENT_STUCK_INIT_THRES(x) (((x) * 9) / 10)
#endif

/***************************************************************************
 * Jitter RNG Configuration Section
 *
 * You may alter the following options
 ***************************************************************************/

/*
 * Enable timer-less timer support with JENT_CONF_ENABLE_INTERNAL_TIMER
 *
 * In case the hardware is identified to not provide a high-resolution time
 * stamp, this option enables a built-in high-resolution time stamp mechanism.
 *
 * The timer-less noise source is based on threads. This noise source requires
 * the linking with the POSIX threads library. I.e. the executing environment
 * must offer POSIX threads. If this option is disabled, no linking
 * with the POSIX threads library is needed.
 */

/*
 * Shall the LAG predictor health test be enabled?
 */
#define JENT_HEALTH_LAG_PREDICTOR

/*
 * Shall the jent_memaccess use a (statistically) random selection for the
 * memory to update?
 */
#ifndef JENT_TEST_MEASURE_RAW_MEMORY_ACCESS
#define JENT_RANDOM_MEMACCESS
#else
#undef JENT_RANDOM_MEMACCESS
#endif

/*
 * Mask specifying the number of bits of the raw entropy data of the time delta
 * value used for the APT.
 *
 * This value implies that for the APT, only the bits specified by
 * JENT_APT_MASK are taken. This was suggested in a draft IG D.K resolution 22
 * provided by NIST, but further analysis
 * (https://www.untruth.org/~josh/sp80090b/CMUF%20EWG%20Draft%20IG%20D.K%20Comments%20D10.pdf)
 * suggests that this truncation / translation generally results in a health
 * test with both a higher false positive rate (because multiple raw symbols
 * map to the same symbol within the health test) and a lower statistical power
 * when the APT cutoff is selected based on the apparent truncated entropy
 * (i.e., truncation generally makes the test worse). NIST has since withdrawn
 * this draft and stated that they will not propose truncation prior to
 * health testing.
 * Because the general tendency of such truncation to make the health test
 * worse the default value is set such that no data is masked out and this
 * should only be changed if a hardware-specific analysis suggests that some
 * other mask setting is beneficial.
 * The mask is applied to a time stamp where the GCD is already divided out, and thus no
 * "non-moving" low-order bits are present.
 */
#define JENT_APT_MASK		(UINT64_C(0xffffffffffffffff))

/*
 * This parameter defines the default memory buffer size for the memory access
 * loop. This value implies a memory size of 1 << JENT_DEFAULT_MEMORY_BITS.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * This parameter is applied if the Jitter RNG:
 * - is not instantiated with JENT_MAX_MEMSIZE_* flag
 * - cannot determine the L1 cache size during starup.
 */
#ifndef JENT_DEFAULT_MEMORY_BITS
# define JENT_DEFAULT_MEMORY_BITS 18
#endif

/* This parameter establishes the multiplicative factor that the desired
 * memory region size should be larger than the observed cache size; the
 * multiplicative factor is 2^JENT_CACHE_SHIFT_BITS.
 * Set this to 0 if the desired memory region should be at least as large as
 * the cache. If one wants most of the memory updates to result in a memory
 * update, then this value should be at least 1.
 * If the memory updates should dominantly result in a memory update, then
 * the value should be set to at least 3.
 * The actual size of the memory region is never larger than requested by
 * the passed in JENT_MAX_MEMSIZE_* flag (if provided) or JENT_MEMORY_SIZE
 * (if no JENT_MAX_MEMSIZE_* flag is provided).
 */
#ifndef JENT_CACHE_SHIFT_BITS
#define JENT_CACHE_SHIFT_BITS 0
#endif

/*
 * Ceiling for the memory size that jent_update_memsize() derives on its own
 * from the discovered cache geometry. It does not constrain a size the caller
 * requested explicitly with a JENT_MAX_MEMSIZE_* flag - that is the caller's
 * decision to make - only the automatic one.
 *
 * On a 64-bit target this is JENT_MAX_MEMSIZE_MAX, i.e. no additional limit.
 * On a 32-bit target the address space is the binding constraint rather than
 * the cache: a two-socket machine with a large L3 makes JENT_CACHE_ALL derive
 * the full 512 MB, which the collector then both maps and mlock()s. That is a
 * sixth of the usable address space of a 32-bit process and well beyond a
 * typical RLIMIT_MEMLOCK, so jent_zalloc() fails and the whole collector
 * allocation fails with it. Capping the derived value at 64 MB keeps the
 * automatic path working on i686, armv7, RV32 and 31-bit s390.
 *
 * UINTPTR_MAX is the pointer-width test; where it is unavailable (the Linux
 * kernel build does not define it) the 64-bit branch is taken, which leaves
 * that configuration's behaviour unchanged.
 */
#if defined(UINTPTR_MAX) && (UINTPTR_MAX <= 0xffffffffUL)
# define JENT_MAX_AUTO_MEMSIZE JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_64MB)
#else
# define JENT_MAX_AUTO_MEMSIZE JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX)
#endif

/*
 * Memory access loop count: This value defines the default memory access loop
 * count. The memory access loop is one of the hearts of the Jitter RNG. The
 * number of loop counts has a direct impact on the entropy rate.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source. Make sure you fully understand what you do. If you want to
 * individually measure the memory access loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --memaccess.
 */
#ifndef JENT_MEM_ACC_LOOP_DEFAULT
#define JENT_MEM_ACC_LOOP_DEFAULT 128
#endif

/*
 * Memory access loop initialization count: This value defines the multiplier of
 * the memory access loop count during initialization phase, when the memory
 * access based loop is the sole entropy provider. Typically a higher iteration
 * count is necessary to take enough time.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source during NTG.1 initialization.
 * Make sure you fully understand what you do. If you want to
 * individually measure the hash loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --hashloop.
 */
#ifndef JENT_MEM_ACC_LOOP_INIT
#define JENT_MEM_ACC_LOOP_INIT 3
#endif

/*
 * Hash loop count: This value defines the default hash loop count. The hash
 * loop is one of the hearts of the Jitter RNG. The number of loop counts has a
 * direct impact on the entropy rate.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * This value is applied if the Jitter RNG:
 * - is not instantiated with JENT_HASHLOOP_* flag
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source. Make sure you fully understand what you do. If you want to
 * individually measure the hash loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --hashloop.
 */
#ifndef JENT_HASH_LOOP_DEFAULT
#define JENT_HASH_LOOP_DEFAULT 1
#endif

/*
 * Hash loop initialization count: This value defines the multiplier of the
 * hash loop count during initialization phase, when the SHA-3-based loop is the
 * sole entropy provider. Typically a higher iteration count is necessary to
 * take enough time.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source during NTG.1 initialization.
 * Make sure you fully understand what you do. If you want to
 * individually measure the hash loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --hashloop.
 */
#ifndef JENT_HASH_LOOP_INIT
#define JENT_HASH_LOOP_INIT 3
#endif

/*
 * Oversampling rate: This value defines the default oversampling rate. The
 * OSR defines the global heuristic entropy rate of 1/OSR.
 *
 * It is permissible to configure this value differently at compile time.
 *
 * This value is applied if the Jitter RNG:
 * - is instantiated with an OSR of 0 provided to the initialization API
 *
 * During initial health tests or jent_read_entropy_safe, the RNG instance
 * may re-initialize with an incremented OSR, which stops at JENT_MAX_OSR
 * and returns a failure condition. Otherwise this would run "forever".
 *
 * JENT_MAX_OSR is what an allocation accepts and what that re-initialization
 * climbs to. It may be raised as far as JENT_HEALTH_CUTOFF_TABLE_OSR, the
 * highest rate the health test cutoff tables carry an entry for: above those
 * the tests would run on the cutoffs of the highest rate they know, which
 * are stricter than the entropy rate of 1/OSR the higher rate claims, and a
 * healthy noise source would fail them. tests/health/cutoffs.py recomputes
 * the tables for a value beyond even that, whose own ceiling is the
 * collection loop - one output block takes (256 + safety factor) * OSR time
 * deltas, counted in the unsigned short window counters of the repetition
 * count test with memory, which caps the rate at 204 in a compliance mode
 * and 255 outside one; the build assertion in jent_random_data_one() holds
 * the value to the former. A high rate costs proportionally, that loop
 * generating that many deltas per output block, and a re-initialization
 * ladder walking a rung per failure up to the maximum.
 *
 * JENT_MAX_OSR_FIPS and JENT_MAX_OSR_NTG1 are the same ceiling for an
 * instance in one of the compliance modes, which is not the same question:
 * what the analysis behind a mode covers is settled when that analysis is
 * made, not when the health tests gain a table entry. Both are 20, where the
 * compliance statements of this library stop, and an allocation asking for
 * more in either mode is refused - so raising JENT_MAX_OSR extends what the
 * library will run at without silently extending what it claims. Neither may
 * exceed JENT_MAX_OSR, which is asserted at build time.
 */
#ifndef JENT_MIN_OSR
#define JENT_MIN_OSR	3
#endif

#ifndef JENT_MAX_OSR
#define JENT_MAX_OSR	20
#endif

#ifndef JENT_MAX_OSR_FIPS
#define JENT_MAX_OSR_FIPS	20
#endif

#ifndef JENT_MAX_OSR_NTG1
#define JENT_MAX_OSR_NTG1	20
#endif

/***************************************************************************
 * Jitter RNG State Definition Section
 ***************************************************************************/

#define JENT_SHA3_256_SIZE_DIGEST_BITS	256
#define JENT_SHA3_256_SIZE_DIGEST	(JENT_SHA3_256_SIZE_DIGEST_BITS >> 3)

/*
 * The output 256 bits can receive more than 256 bits of min entropy,
 * of course, but the 256-bit output of XDRBG-256(M) can only
 * asymptotically approach 256 bits of min entropy, not attain that bound.
 * Random maps will tend to have output collisions, which reduces the creditable
 * output entropy (that is what SP 800-90B Section 3.1.5.1.2 attempts to bound).
 *
 * The value "64" is justified in Appendix A.4 of the current 90C draft,
 * and aligns with NIST's in "epsilon" definition in this document, which is
 * that a string can be considered "full entropy" if you can bound the min
 * entropy in each bit of output to at least 1-epsilon, where epsilon is
 * required to be <= 2^(-32).
 *
 * The additional bit for the safety factor comes from the fact that the
 * we want the internal state variable to have 256 + 64 bit of entropy. As this
 * state variable is generated by one SHAKE256 operation of the input data, we
 * loose one bit of entropy due to the SHAKE. For more details, see the NTG.1
 * analysis provided with the big documentation.
 */
#define ENTROPY_SAFETY_FACTOR		(64 + 1)

enum jent_startup_state {
	jent_startup_completed,
	jent_startup_sha3,
	jent_startup_memory
};

/* The entropy pool */
struct rand_data
{
	/* all data values that are vital to maintain the security
	 * of the RNG are marked as SENSITIVE. A user must not
	 * access that information while the RNG executes its loops to
	 * calculate the next random value. */
	void *hash_state;		/* SENSITIVE hash state entropy pool */
	uint64_t prev_time;		/* SENSITIVE Previous time stamp */
#define DATA_SIZE_BITS (JENT_SHA3_256_SIZE_DIGEST_BITS)

#ifndef JENT_HEALTH_LAG_PREDICTOR
	uint64_t last_delta;		/* SENSITIVE stuck test */
	uint64_t last_delta2;		/* SENSITIVE stuck test */
#endif /* JENT_HEALTH_LAG_PREDICTOR */

	unsigned int flags;		/* Flags used to initialize */
	unsigned int osr;		/* Oversampling rate */

	/* RFC 4122 version 4 identifier, stable for the collector's lifetime. */
	char uuid[JENT_UUID_STRLEN];

	/*
	 * Number of times this instance has been reinitialized (reallocated on
	 * health-test recovery). Carried over, incremented, across the identity-
	 * preserving reallocation in jent_health_failure_reset().
	 */
	unsigned int reinit_count;

	/*
	 * Per-instance output accounting, reported by jent_status(). Both are
	 * carried over across the identity-preserving reallocation in
	 * jent_health_failure_reset() so the totals span the instance's whole
	 * lifetime.
	 *
	 * read_invocations counts caller read requests: jent_read_entropy()
	 * increments it only on success, so a jent_read_entropy_safe() request
	 * maps to one invocation regardless of how many internal health-test
	 * retries it takes (failed attempts never count).
	 * bytes_output counts the random bytes actually delivered to callers.
	 */
	uint64_t read_invocations;
	uint64_t bytes_output;

	/* Initialization state supporting AIS 20/31 NTG.1 */
	enum jent_startup_state startup_state;

/* The step size should be larger than the cacheline size. */
#ifndef JENT_MEMORY_BLOCKSIZE
# define JENT_MEMORY_BLOCKSIZE 128
#endif
#define JENT_MEMORY_BLOCKS(ec) ((ec->memmask + 1) / JENT_MEMORY_BLOCKSIZE)

	unsigned char *mem;		/* Memory access location with size of
					 * memmask + 1 */

	uint32_t memmask;		/* Memory mask (size of memory - 1) */
	unsigned int memlocation; 	/* Pointer to byte in *mem */
	unsigned int memaccessloops;	/* Number of memory accesses per random
					 * bit generation */

	unsigned int hashloopcnt;	/* Hash loop count */

	/* Repetition Count Test */
	unsigned int rct_count;		/* Number of stuck values */
	unsigned short rct_cutoff;	/* RCT intermittent cutoff */
	unsigned short rct_cutoff_permanent; /* RCT permanent cutoff */

	/* Adaptive Proportion Test for a significance level of 2^-30 */
	unsigned int apt_cutoff;	/* Intermittent health test failure */
	unsigned int apt_cutoff_permanent; /* Permanent health test failure */
#define JENT_APT_WINDOW_SIZE	512	/* Data window size */
	unsigned int apt_observations;	/* Number of collected observations in
					 * current window. */
	unsigned int apt_count;		/* The number of times the reference
					 * symbol been encountered in the
					 * window. */
	uint64_t apt_base;		/* APT base reference */
	unsigned int health_failure;	/* Permanent health failure */

	/* RCT with memory */
	unsigned short rct_mem_ctr;	/* Loop iteration for generating random bytes */
	unsigned short rct_mem_nosr;	/* Minimum iteration count of measure jitter loop */
	unsigned short rct_mem_count;	/* Number of stuck values */
	unsigned short rct_mem_cutoff;	/* RCT intermittent cutoff */
	unsigned short rct_mem_cutoff_permanent; /* RCT permanent cutoff */

	unsigned int apt_base_set:1;	/* APT base reference set? */
	unsigned int is_fips_enabled:1;
	unsigned int enable_notime:1;	/* Use internal high-res timer */
	unsigned int max_mem_set:1;	/* Maximum memory configured by user */
	unsigned int in_recovery:1;	/* Flag to indicate a recovery op. */

	/*
	 * A jent_selftest() run bound to this instance failed. Deliberately
	 * not a bit in health_failure: that word only reports under FIPS,
	 * while a broken conditioning component must stop the output in every
	 * mode. A full word rather than a bitfield: the self test may run on
	 * another thread, and setting a bitfield would rewrite its neighbors.
	 */
	unsigned int selftest_failed:1;

	/*
	 * A collection loop returned without collecting: the clock stopped
	 * advancing, or the window it would have used is out of range. Outside
	 * health_failure for the reason selftest_failed is - that word reports
	 * only under FIPS, and a pool that was never fed must stop the output
	 * in every mode. Set by jent_random_data_one() alone.
	 */
	unsigned int noise_stopped:1;

	/*
	 * jent_read_entropy_safe() has given up recovering this collector:
	 * it is at the highest oversampling rate it may run at - JENT_MAX_OSR,
	 * or the ceiling of its compliance mode - so the reallocation cannot
	 * be attempted again, and the health failure that ended the recovery
	 * is sticky.
	 * Every further read would generate an output block only to discard
	 * it and report the same failure, so the reads report it without
	 * spending the block. Set by jent_health_failure_reset() alone.
	 */
	unsigned int recovery_exhausted:1;

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	volatile uint8_t notime_interrupt;	/* indicator to interrupt ctr */
	volatile uint64_t notime_timer;		/* high-res timer mock-up */
	uint64_t notime_prev_timer;		/* previous timer value */
	void *notime_thread_ctx;		/* register thread data */
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	uint64_t jent_common_timer_gcd;	/* Common divisor for all time deltas */

#ifdef JENT_HEALTH_LAG_PREDICTOR
	/* Lag predictor test to look for re-occurring patterns. */

	/* The lag global cutoff selected based on the selection of osr. */
	unsigned int lag_global_cutoff;

	/* The lag global cutoff for a permanent health test failure. */
	unsigned int lag_global_cutoff_permanent;

	/* The lag local cutoff selected based on the selection of osr. */
	unsigned int lag_local_cutoff;

	/* The lag local cutoff for a permanent health test failure. */
	unsigned int lag_local_cutoff_permanent;

	/*
	 * The number of times the lag predictor was correct. Compared to the
	 * global cutoff.
	 */
	unsigned int lag_prediction_success_count;

	/*
	 * The size of the current run of successes. Compared to the local
	 * cutoff.
	 */
	unsigned int lag_prediction_success_run;

	/*
	 * The total number of collected observations since the health test was
	 * last reset.
	 */
	unsigned int lag_best_predictor;

	/*
	 * The total number of collected observations since the health test was
	 * last reset.
	 */
	unsigned int lag_observations;

	/*
	 * This is the size of the window used by the predictor. The predictor
	 * is reset between windows.
	 */
#define JENT_LAG_WINDOW_SIZE (1U<<17)

	/*
	 * The amount of history to base predictions on. This must be a power
	 * of 2. Must be 4 or greater.
	 */
#define JENT_LAG_HISTORY_SIZE 8
#define JENT_LAG_MASK (JENT_LAG_HISTORY_SIZE - 1)

	/* The delta history for the lag predictor. */
	uint64_t lag_delta_history[JENT_LAG_HISTORY_SIZE];

	/* The scoreboard that tracks how successful each predictor lag is. */
	unsigned int lag_scoreboard[JENT_LAG_HISTORY_SIZE];
#endif /* JENT_HEALTH_LAG_PREDICTOR */
};

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_INTERNAL_H */
