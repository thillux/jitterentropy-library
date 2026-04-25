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
 * OS-specific online CPU count discovery.
 *
 * Provides jent_ncpu() returning the number of online logical CPUs, or
 * a negative errno on failure. The dispatch is:
 *   - Windows -> GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
 *   - POSIX / Apple -> sysconf(_SC_NPROCESSORS_ONLN)
 *   - other   -> 1 (timer thread will be disabled)
 */

#ifndef _JITTERENTROPY_ARCH_NCPU_H
#define _JITTERENTROPY_ARCH_NCPU_H

#include <errno.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_NCPU_WINDOWS
#elif defined(_POSIX_SOURCE) || defined(__APPLE__)
# include <unistd.h>
# define JENT_ARCH_NCPU_POSIX
#endif

static inline long jent_ncpu(void)
{
#if defined(JENT_ARCH_NCPU_WINDOWS)
	return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#elif defined(JENT_ARCH_NCPU_POSIX)
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	if (ncpu == -1)
		return -errno;

	if (ncpu == 0)
		return -EFAULT;

	return ncpu;
#else
	/*
	 * TODO: return number of available CPUs -
	 * this code disables timer thread as only one CPU is "detected".
	 */
	return 1;
#endif
}

#endif /* _JITTERENTROPY_ARCH_NCPU_H */
