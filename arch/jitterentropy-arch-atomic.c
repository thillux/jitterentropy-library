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
 * Dispatch:
 *   - Linux kernel                -> smp_load_acquire() / smp_store_release()
 *   - baremetal without lock-free -> volatile access and compiler barriers
 *     32-bit / pointer atomics        (single core, see below)
 *     (ARMv6-M, RISC-V without A)
 *   - GCC / Clang (any target,    -> __atomic_load_n() / __atomic_store_n()
 *     the FreeBSD kernel and the      with __ATOMIC_ACQUIRE / _RELEASE
 *     other baremetal builds
 *     included)
 *   - MSVC                        -> the Interlocked intrinsics
 *   - anything else               -> volatile access
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#if defined(LINUX_KERNEL) || defined(__KERNEL__)

#include <asm/barrier.h>
#include <linux/atomic.h>

int jent_atomic_load_int(const int *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_int(int *ptr, int val)
{
	smp_store_release(ptr, val);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return smp_load_acquire(ptr);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	uint32_t old;

	do {
		old = READ_ONCE(*ptr);
	} while (cmpxchg(ptr, old, old + 1) != old);

	return old + 1;
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	smp_store_release(ptr, val);
}

jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	smp_store_release(ptr, val);
}

#elif defined(JENT_BAREMETAL) && \
      (defined(__GNUC__) || defined(__clang__)) && \
      defined(__GCC_ATOMIC_INT_LOCK_FREE) && \
      defined(__GCC_ATOMIC_POINTER_LOCK_FREE) && \
      (__GCC_ATOMIC_INT_LOCK_FREE < 2 || __GCC_ATOMIC_POINTER_LOCK_FREE < 2)

/*
 * A baremetal target whose instruction set has no lock-free 32-bit atomics:
 * ARMv6-M (Cortex-M0/M0+/M1) has no LDREX/STREX, a RISC-V core without the A
 * extension no AMOs or LR/SC. The compiler lowers every __atomic builtin there
 * - the plain loads and stores included, on arm-none-eabi - to a call to
 * __atomic_load_4() / __atomic_fetch_add_4() and friends, which live in
 * libatomic, and a freestanding link has none: the build fails on undefined
 * symbols.
 *
 * Such a core is a single processor, and aligned word loads and stores are
 * single-copy atomic on it, so a volatile access ordered by a compiler barrier
 * is all acquire and release semantics amount to. The one read-modify-write,
 * jent_atomic_inc_u32(), is not atomic: its only caller is
 * jent_uuid_from_counter(), the baremetal instance identifier, and the
 * increment is safe as long as collectors are not allocated from an interrupt
 * handler while the main line allocates one as well. An integrator who needs
 * that masks interrupts around jent_entropy_collector_alloc(); making the
 * increment itself interrupt-safe would take the very primitive the target
 * lacks, or the platform's interrupt mask, which this library cannot reach.
 */

#define JENT_ATOMIC_BARRIER()	__asm__ __volatile__("" ::: "memory")

int jent_atomic_load_int(const int *ptr)
{
	int val = *(const volatile int *)ptr;

	JENT_ATOMIC_BARRIER();
	return val;
}

void jent_atomic_store_int(int *ptr, int val)
{
	JENT_ATOMIC_BARRIER();
	*(volatile int *)ptr = val;
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	uint32_t val = *(const volatile uint32_t *)ptr;

	JENT_ATOMIC_BARRIER();
	return val;
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	uint32_t val;

	JENT_ATOMIC_BARRIER();
	val = *(volatile uint32_t *)ptr + 1;
	*(volatile uint32_t *)ptr = val;
	JENT_ATOMIC_BARRIER();

	return val;
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	JENT_ATOMIC_BARRIER();
	*(volatile uint32_t *)ptr = val;
}

jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr)
{
	jent_fnptr val = *(const jent_fnptr volatile *)ptr;

	JENT_ATOMIC_BARRIER();
	return val;
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	JENT_ATOMIC_BARRIER();
	*(jent_fnptr volatile *)ptr = val;
}

#undef JENT_ATOMIC_BARRIER

#elif defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))

int jent_atomic_load_int(const int *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_int(int *ptr, int val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

/* The builtins take any scalar, a pointer to a function included. */
jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

#elif defined(_MSC_VER)

#include <intrin.h>

/*
 * The loads are an OR of 0 - a read-modify-write that stores back what it read
 * - because that is the one Interlocked form that is a full barrier on every
 * target MSVC builds for: a volatile read is an acquire on x86 and x64 but not
 * on Arm64, where /volatile:iso is the default. The const the loads take is
 * cast away for that reason alone. No object handed to them is defined const -
 * they are the library's own writable latches, seen through a const pointer by
 * a reader such as jent_status() - so the store never meets read-only memory.
 */
int jent_atomic_load_int(const int *ptr)
{
	return (int)_InterlockedOr((volatile long *)ptr, 0);
}

void jent_atomic_store_int(int *ptr, int val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return (uint32_t)_InterlockedOr((volatile long *)ptr, 0);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	return (uint32_t)_InterlockedIncrement((volatile long *)ptr);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr)
{
	return (jent_fnptr)(uintptr_t)_InterlockedCompareExchangePointer(
		(void * volatile *)ptr, NULL, NULL);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	(void)_InterlockedExchangePointer((void * volatile *)ptr,
					  (void *)(uintptr_t)val);
}

#else /* no atomics available */

/*
 * The one read-modify-write: the latches below tolerate a plain volatile
 * access, a counter handed out to concurrent callers does not. C11 atomics
 * where the compiler has them (xlc, Oracle Studio); the cast assumes a
 * lock-free 32-bit atomic shares the plain type's representation.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_ATOMICS__)
# include <stdatomic.h>
# define JENT_ATOMIC_C11_INC
#endif

int jent_atomic_load_int(const int *ptr)
{
	return *(const volatile int *)ptr;
}

void jent_atomic_store_int(int *ptr, int val)
{
	*(volatile int *)ptr = val;
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return *(const volatile uint32_t *)ptr;
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
#ifdef JENT_ATOMIC_C11_INC
	return atomic_fetch_add((_Atomic uint32_t *)ptr, 1) + 1;
#else
	uint32_t val = *(volatile uint32_t *)ptr + 1;

	*(volatile uint32_t *)ptr = val;
	return val;
#endif
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	*(volatile uint32_t *)ptr = val;
}

jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr)
{
	return *(const jent_fnptr volatile *)ptr;
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	*(jent_fnptr volatile *)ptr = val;
}

#endif
