/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2013 - 2025
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

/*
 * Architecture-specific high-resolution timestamp source.
 *
 * Provides jent_get_nstime() implemented via the highest-resolution counter
 * available on the target. The dispatch order is:
 *
 *   - Windows (MSVC / MinGW):
 *       * ARM / ARM64 -> QueryPerformanceCounter()
 *       * otherwise   -> __rdtsc() intrinsic
 *   - x86 / x86_64       -> rdtsc inline asm
 *   - aarch64            -> mrs <reg> (cntvct_el0 by default), or
 *                           clock_gettime_nsec_np(CLOCK_UPTIME_RAW) on Apple
 *   - s390x              -> stcke
 *   - powerpc            -> mftbu/mftb (or mfspr on newer cores)
 *   - generic fallback   -> mach_absolute_time() on Mach,
 *                           read_real_time() on AIX,
 *                           clock_gettime(CLOCK_REALTIME) elsewhere
 */

#ifndef _JITTERENTROPY_ARCH_TIMER_H
#define _JITTERENTROPY_ARCH_TIMER_H

#include <stdint.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# define JENT_ARCH_TIMER_WINDOWS
# include <windows.h>
# include <intrin.h>
# if defined(_M_ARM) || defined(_M_ARM64)
#  include <profileapi.h>
#  define JENT_ARCH_TIMER_WINDOWS_QPC
# else
#  define JENT_ARCH_TIMER_WINDOWS_RDTSC
# endif

#elif defined(__x86_64__) || defined(__i386__)
# define JENT_ARCH_TIMER_X86

# ifdef __x86_64__
/* specify 64 bit type since long is 32 bits in LLP64 x86_64 systems */
#  define DECLARE_ARGS(val, low, high)    uint64_t low, high
#  define EAX_EDX_VAL(val, low, high)     ((low) | ((high) << 32))
#  define EAX_EDX_RET(val, low, high)     "=a" (low), "=d" (high)
# else /* __i386__ */
#  define DECLARE_ARGS(val, low, high)    unsigned long long val
#  define EAX_EDX_VAL(val, low, high)     (val)
#  define EAX_EDX_RET(val, low, high)     "=A" (val)
# endif

#elif defined(__aarch64__)
# define JENT_ARCH_TIMER_AARCH64
# ifndef AARCH64_NSTIME_REGISTER
#  define AARCH64_NSTIME_REGISTER "cntvct_el0"
# endif
# ifdef __MACH__
/*
 * On modern Apple platforms (M1+), the system counter is too coarse.
 * Use clock_gettime_nsec_np(CLOCK_UPTIME_RAW) instead.
 */
#  include <time.h>
#  define JENT_ARCH_TIMER_AARCH64_APPLE
# endif

#elif defined(__s390x__)
# define JENT_ARCH_TIMER_S390X

#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_TIMER_POWERPC
/*
 * Uncomment this for newer PPC CPUs.
 * Newer PPC CPUs do not support mftbu/mftb; these instructions were
 * obsoleted and replaced by mfspr.  Special processor registers 268
 * and 269 are the ones we want.
 */
/* #define POWER_PC_USE_NEW_INSTRUCTIONS */

#else /* generic fallback */
# define JENT_ARCH_TIMER_GENERIC
# include <time.h>
# ifdef __MACH__
#  include <mach/mach_time.h>
# endif
#endif

static inline void jent_get_nstime(uint64_t *out)
{
#if defined(JENT_ARCH_TIMER_WINDOWS_QPC)

	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	*out = (uint64_t)ticks.QuadPart;

#elif defined(JENT_ARCH_TIMER_WINDOWS_RDTSC)

	*out = __rdtsc();

#elif defined(JENT_ARCH_TIMER_X86)

	DECLARE_ARGS(val, low, high);
# ifdef __sun__
	__asm("rdtsc" : EAX_EDX_RET(val, low, high));
# else
	__asm__ __volatile__("rdtsc" : EAX_EDX_RET(val, low, high));
# endif
	*out = EAX_EDX_VAL(val, low, high);

#elif defined(JENT_ARCH_TIMER_AARCH64_APPLE)

	*out = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

#elif defined(JENT_ARCH_TIMER_AARCH64)

	uint64_t ctr_val;
	__asm__ __volatile__("mrs %0, " AARCH64_NSTIME_REGISTER : "=r" (ctr_val));
	*out = ctr_val;

#elif defined(JENT_ARCH_TIMER_S390X)

	/*
	 * GCC + STCKE. STCKE command and data format:
	 * z/Architecture - Principles of Operation
	 * http://publibz.boulder.ibm.com/epubs/pdf/dz9zr007.pdf
	 *
	 * The current value of bits 0-103 of the TOD clock is stored in
	 * bytes 1-13 of the sixteen-byte output. Bit 59 (TOD-Clock bit 51)
	 * effectively increments every microsecond; the stepping value of
	 * TOD-clock bit 63 is approximately 244 picoseconds.
	 */
	uint8_t clk[16];

	__asm__ __volatile__("stcke %0" : "=Q" (clk) : : "cc");

	/* s390x is big-endian, so just perform a byte-by-byte copy */
	*out = *(uint64_t *)(clk + 1);

#elif defined(JENT_ARCH_TIMER_POWERPC)

	/* taken from http://www.ecrypt.eu.org/ebats/cpucycles.html */
	unsigned long high;
	unsigned long low;
	unsigned long newhigh;
	uint64_t result;

# ifdef POWER_PC_USE_NEW_INSTRUCTIONS
	__asm__ __volatile__(
		"Lcpucycles:mfspr %0, 269;mfspr %1, 268;mfspr %2, 269;cmpw %0,%2;bne Lcpucycles"
		: "=r" (high), "=r" (low), "=r" (newhigh)
		);
# else
	__asm__ __volatile__(
		"Lcpucycles:mftbu %0;mftb %1;mftbu %2;cmpw %0,%2;bne Lcpucycles"
		: "=r" (high), "=r" (low), "=r" (newhigh)
		);
# endif
	result = high;
	result <<= 32;
	result |= low;
	*out = result;

#else /* JENT_ARCH_TIMER_GENERIC */

# ifdef __MACH__
	/*
	 * macOS lacks clock_gettime on older releases. Taken from
	 * http://developer.apple.com/library/mac/qa/qa1398/_index.html
	 */
	*out = mach_absolute_time();
# elif defined(_AIX)
	/*
	 * clock_gettime() on AIX returns a timer value that increments in
	 * steps of 1000.
	 */
	uint64_t tmp = 0;
	timebasestruct_t aixtime;
	read_real_time(&aixtime, TIMEBASE_SZ);
	time_base_to_time(&aixtime, TIMEBASE_SZ);
	tmp = (uint64_t)aixtime.tb_high * 1000000000UL;
	tmp += (uint64_t)aixtime.tb_low;
	*out = tmp;
# else
	/*
	 * We could use CLOCK_MONOTONIC(_RAW), but with CLOCK_REALTIME we
	 * pick up some nice extra entropy once in a while from NTP.
	 */
	uint64_t tmp = 0;
	struct timespec time;

	if (clock_gettime(CLOCK_REALTIME, &time) == 0) {
		tmp = ((uint64_t)time.tv_sec & 0xFFFFFFFF) * 1000000000UL;
		tmp = tmp + (uint64_t)time.tv_nsec;
	}
	*out = tmp;
# endif

#endif
}

#endif /* _JITTERENTROPY_ARCH_TIMER_H */
