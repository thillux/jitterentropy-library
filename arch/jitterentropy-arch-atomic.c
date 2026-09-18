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
 *   - GCC / Clang (any target,    -> __atomic_load_n() / __atomic_store_n()
 *     the FreeBSD kernel and the      with __ATOMIC_ACQUIRE / _RELEASE
 *     baremetal builds included)
 *   - MSVC                        -> the Interlocked intrinsics
 *   - anything else               -> volatile access
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#if defined(LINUX_KERNEL) || defined(__KERNEL__)

#include <asm/barrier.h>
#include <linux/atomic.h>

int jent_atomic_load_int(int *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_int(int *ptr, int val)
{
	smp_store_release(ptr, val);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
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

jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	smp_store_release(ptr, val);
}

#elif defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))

int jent_atomic_load_int(int *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_int(int *ptr, int val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
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
jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

#elif defined(_MSC_VER)

#include <intrin.h>

int jent_atomic_load_int(int *ptr)
{
	return (int)_InterlockedOr((volatile long *)ptr, 0);
}

void jent_atomic_store_int(int *ptr, int val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
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

jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
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

int jent_atomic_load_int(int *ptr)
{
	return *(volatile int *)ptr;
}

void jent_atomic_store_int(int *ptr, int val)
{
	*(volatile int *)ptr = val;
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return *(volatile uint32_t *)ptr;
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

jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return *(jent_fnptr volatile *)ptr;
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	*(jent_fnptr volatile *)ptr = val;
}

#endif
