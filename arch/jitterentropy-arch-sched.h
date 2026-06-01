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

/*
 * Architecture / OS-specific scheduler yield.
 *
 * Provides jent_yield() which combines a CPU-level pause hint (to ease
 * SMT and power contention while the caller is busy-waiting) with an
 * OS-level scheduler yield. The two phases are dispatched independently.
 *
 * CPU pause hint:
 *   - x86 / x86_64           -> _mm_pause() intrinsic
 *   - aarch64                -> 'yield' instruction
 *   - arm (ARMv7+)           -> 'yield' instruction
 *   - powerpc                -> 'or 27,27,27' (low-priority hint)
 *   - other (s390x, riscv,   -> no hint
 *     unknown)
 *
 * OS yield:
 *   - Windows (MSVC / MinGW)             -> SwitchToThread()
 *   - hosted Unix-like (Linux, BSDs,     -> sched_yield()
 *     Apple, AIX, Solaris/illumos,
 *     Haiku, Cygwin)
 *   - other (e.g. baremetal)             -> no-op
 *
 * The CPU hint mirrors what YieldProcessor() does on Windows (which
 * expands to the architecture's pause/yield instruction). On baremetal
 * targets with no scheduler we still emit the CPU hint so a busy-wait
 * loop does not pin SMT siblings unnecessarily.
 */

#ifndef _JITTERENTROPY_ARCH_SCHED_H
#define _JITTERENTROPY_ARCH_SCHED_H

#ifdef __KERNEL__

# include <linux/sched.h>

/*
 * cpu_relax() is the kernel's CPU-pause hint (same pause/yield
 * instructions used in userspace); cond_resched() lets the scheduler
 * swap us out if a higher-priority task is waiting. The notime
 * busy-wait calls this on every spin, but the read paths are sleepable
 * (misc cdev read, hwrng fill thread).
 */
static inline void jent_yield(void)
{
	cpu_relax();
	cond_resched();
}

#elif defined(_KERNEL) && defined(__FreeBSD__)

# include <machine/cpu.h>

/*
 * cpu_spinwait() emits the architecture's pause/yield hint. The
 * FreeBSD scheduler preempts us normally on its own ticks, so no
 * explicit kern_yield() is needed in this tight spin.
 */
static inline void jent_yield(void)
{
	cpu_spinwait();
}

#elif defined(JENT_BAREMETAL) || \
      (defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0))

/*
 * Baremetal: no scheduler to yield to, just emit the architecture's
 * pause/yield hint to ease SMT contention on the spin.
 */
static inline void jent_yield(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || \
      (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
	__asm__ __volatile__("yield" ::: "memory");
#elif defined(__powerpc) || defined(__powerpc__)
	__asm__ __volatile__("or 27,27,27" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

#else /* hosted userspace */

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_SCHED_OS_WINDOWS
#elif defined(__unix__) || defined(__APPLE__) || defined(_AIX) || \
      defined(__sun)    || defined(__HAIKU__) || defined(__CYGWIN__)
# include <sched.h>
# define JENT_ARCH_SCHED_OS_POSIX
#endif

#if defined(__x86_64__) || defined(__i386__) || \
    defined(_M_X64)     || defined(_M_IX86)
# if defined(_MSC_VER)
#  include <intrin.h>
# else
#  include <x86intrin.h>
# endif
# define JENT_ARCH_SCHED_PAUSE_X86
#elif defined(__aarch64__) || \
      (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
# define JENT_ARCH_SCHED_PAUSE_ARM
#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_SCHED_PAUSE_POWERPC
#endif

static inline void jent_yield(void)
{
#if defined(JENT_ARCH_SCHED_PAUSE_X86)
	_mm_pause();
#elif defined(JENT_ARCH_SCHED_PAUSE_ARM)
	__asm__ __volatile__("yield" ::: "memory");
#elif defined(JENT_ARCH_SCHED_PAUSE_POWERPC)
	__asm__ __volatile__("or 27,27,27" ::: "memory");
#endif

#if defined(JENT_ARCH_SCHED_OS_WINDOWS)
	SwitchToThread();
#elif defined(JENT_ARCH_SCHED_OS_POSIX)
	(void)sched_yield();
#endif
}

#endif /* any kernel vs userspace */

#endif /* _JITTERENTROPY_ARCH_SCHED_H */
