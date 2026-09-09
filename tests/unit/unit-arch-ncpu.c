/*
 * Jitter RNG: unit tests for the platform backends in arch/
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
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
 * Jitter RNG: unit tests for the CPU count backend.
 *
 * Every assertion has to hold on every platform: what is checked is the
 * contract the backend header in arch/ states, not the behaviour of one
 * implementation.
 */

/*
 * As in the AMALGAMATED programs under tests/raw-entropy: several arch sources
 * are absorbed here, and the ones needing _GNU_SOURCE define it themselves
 * before their own includes - which is too late once an earlier source in this
 * translation unit has already pulled the headers in. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

static void test_ncpu(void)
{
	long ncpu = jent_ncpu();

	jent_ut_group("jent_ncpu");

	/*
	 * A negative value is the documented "cannot tell" answer; anything
	 * else must be a count that makes sense for a machine this code is
	 * running on.
	 */
	jent_ut_checks++;
	if (ncpu < 0)
		printf("  note: the CPU count is not discoverable here\n");
	else if (ncpu < 1)
		JENT_UT_FAIL("jent_ncpu returned %ld", ncpu);
	else
		printf("  note: %ld CPUs\n", ncpu);
}

#if defined(JENT_ARCH_NCPU_LINUX_SYSFS)
static void test_ncpu_parse(void)
{
	static const struct {
		const char *list;
		long want;
		const char *what;
	} cases[] = {
		{ "0\n",		1,	"a single CPU" },
		{ "0-3\n",		4,	"one contiguous range" },
		{ "0,2-5,8\n",		6,	"ranges with holes" },
		{ "0-0\n",		1,	"a range of one" },
		{ "\n",		-EINVAL, "an empty list" },
		{ "",			-EINVAL, "no list at all" },
		{ "x\n",		-EINVAL, "a non-numeric list" },
		{ "-1\n",		-EINVAL, "a negative CPU number" },
		{ "3-1\n",		-EINVAL, "a range that runs backwards" },
		{ "0-\n",		-EINVAL, "a range with no end" },
		/*
		 * Parses, but names more CPUs than a signed long can count -
		 * rejected before the width is added rather than overflowed
		 * into it.
		 */
		{ "0-9223372036854775807\n",
					-EINVAL, "a range wider than a CPU set" },
	};
	size_t i;
	char path[] = "/tmp/jent-ncpu-XXXXXX";
	int fd;

	jent_ut_group("the online CPU list");

	for (i = 0; i < JENT_ARRAY_SIZE(cases); i++)
		JENT_UT_EQ(jent_ncpu_parse_online(cases[i].list), cases[i].want,
			   cases[i].what);

	/* And the reading of it. */
	JENT_UT_TRUE(jent_ncpu_sysfs_file("/nonexistent/jent/online") < 0,
		     "an unreadable list is an error");

	fd = mkstemp(path);
	if (fd < 0) {
		JENT_UT_SKIP("an empty online list", "no temporary file");
		return;
	}
	close(fd);
	JENT_UT_TRUE(jent_ncpu_sysfs_file(path) < 0,
		     "an empty list file is an error");

	{
		FILE *f = fopen(path, "w");

		if (f) {
			fputs("0-7\n", f);
			fclose(f);
			JENT_UT_EQ(jent_ncpu_sysfs_file(path), 8,
				   "a list of eight CPUs is counted");
		}
	}
	remove(path);
}
#else
static void test_ncpu_parse(void)
{
	JENT_UT_SKIP("the online CPU list", "not the sysfs CPU backend");
}
#endif

#ifdef JENT_ARCH_NCPU_WINDOWS
/* The flat CPU number of @bit in @group, or -1 when no CPU has it. */
static long ut_flat_cpu(unsigned short group, unsigned int bit)
{
	DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	unsigned long cpu;

	for (cpu = 0; cpu < n; cpu++) {
		unsigned short g;
		unsigned int b;

		if (!jent_cpu_to_group(cpu, &g, &b) && g == group && b == bit)
			return (long)cpu;
	}
	return -1;
}

/*
 * Under a narrowed process affinity mask the CPU count and the highest CPU
 * follow the mask, and the pin refuses a CPU outside it, which
 * SetThreadGroupAffinity() alone would move the thread to. The mask is
 * restored before returning.
 */
static void test_ncpu_windows_affinity(void)
{
	HANDLE proc = GetCurrentProcess();
	DWORD_PTR pmask, smask;
	GROUP_AFFINITY ga;
	unsigned int lo, hi, bits = 0;
	long flat_lo, flat_hi;

	jent_ut_group("the Windows CPU count under a process affinity mask");

	if (!GetProcessAffinityMask(proc, &pmask, &smask) || !pmask ||
	    !GetThreadGroupAffinity(GetCurrentThread(), &ga)) {
		JENT_UT_SKIP("the affinity mask", "it cannot be read");
		return;
	}

	for (lo = 0; !((pmask >> lo) & 1); lo++)
		;
	for (hi = (unsigned int)(sizeof(pmask) * 8) - 1; !((pmask >> hi) & 1);
	     hi--)
		;
	for (smask = pmask; smask; smask &= smask - 1)
		bits++;

	JENT_UT_EQ(jent_ncpu(), (long)bits,
		   "the count is the CPUs the process may run on");

	if (bits < 2) {
		JENT_UT_SKIP("a narrowed mask", "the process has one CPU");
		return;
	}

	flat_lo = ut_flat_cpu(ga.Group, lo);
	flat_hi = ut_flat_cpu(ga.Group, hi);
	JENT_UT_TRUE(flat_lo >= 0 && flat_hi > flat_lo,
		     "both ends of the mask have a CPU number");
	JENT_UT_EQ(jent_cpu_highest(), flat_hi,
		   "the highest CPU is the top of the mask");

	if (!SetProcessAffinityMask(proc, (DWORD_PTR)1 << lo)) {
		JENT_UT_SKIP("a narrowed mask", "the mask cannot be changed");
		return;
	}

	JENT_UT_EQ(jent_ncpu(), 1, "confined to one CPU, the count is one");
	JENT_UT_EQ(jent_cpu_highest(), flat_lo,
		   "and the highest CPU is that one");

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	JENT_UT_EQ(jent_thread_pin_to_cpu((unsigned long)flat_hi), -EINVAL,
		   "a CPU outside the process affinity is refused");
	JENT_UT_TRUE(GetThreadGroupAffinity(GetCurrentThread(), &ga) &&
		     ga.Mask == ((KAFFINITY)1 << lo),
		     "and the thread stays inside it");
	JENT_UT_EQ(jent_thread_pin_to_cpu((unsigned long)flat_lo), 0,
		   "the CPU inside it is pinned to");
#endif

	JENT_UT_TRUE(SetProcessAffinityMask(proc, pmask),
		     "the process affinity mask is restored");
	JENT_UT_EQ(jent_ncpu(), (long)bits, "and the count follows it back");
}
#else
static void test_ncpu_windows_affinity(void)
{
	JENT_UT_SKIP("the Windows CPU count under a process affinity mask",
		     "not the Windows CPU backend");
}
#endif

/* The CSPRNG read behind the UUID, against files with known behaviour. */

int main(void)
{
	test_ncpu();
	test_ncpu_parse();
	test_ncpu_windows_affinity();

	return jent_ut_report("unit-arch-ncpu");
}
