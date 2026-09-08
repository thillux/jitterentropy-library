/*
 * Jitter RNG: what a noise source run leaves on the stack
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
 * Every entry point that runs the noise source clears the stack it ran over -
 * see jent_stack_scrub() in src/jitterentropy-base.c. This checks that it
 * happens, on each of those entry points, and that the wipe still reaches
 * deeper than the path does on the machine the test is built for.
 *
 * A slab is painted from a frame deeper than the one the library is called
 * from, so the library runs over it, and is read back afterwards; reading a
 * returned frame is the liberty taken here, and it is the view a core dump
 * would give. The mocked time source gives every stamp a magic nothing else
 * holds, which is what makes a find attributable. A byte still holding the
 * paint was never written.
 *
 * Three things are checked of each entry point, and it takes all three:
 *
 *   - the wipe left JENT_STACK_SCRUB_LEN contiguous zero bytes, and no
 *     non-zero byte among them. That says it ran, and ran in full.
 *   - the band below the wipe still holds nothing but paint. That says the
 *     path did not outgrow what the wipe covers - which the band above cannot
 *     say, every byte in it being written by the wipe itself.
 *   - no raw time stamp survives anywhere on the slab.
 *
 * Which one catches a regression depends on the regression: a wipe that stops
 * happening shows up in the first, a path that grows past
 * JENT_STACK_SCRUB_LEN only ever in the second.
 *
 * The startup entry points are the ones where a raw time stamp actually
 * survives without the wipe - jent_time_entropy_init() returns straight from
 * the measurement loop, while a generate ends in the conditioning pass, whose
 * frames overwrite the collection frames on the way out. The stamp check is
 * run on all of them regardless: which path happens to bury its stamps is a
 * property of frame layout, and frame layout is exactly what changes under a
 * different compiler or target.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <stdlib.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"
#include "jitterentropy-uuid.c"
#include "jitterentropy-status.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * Room for the wipe, for a look below it, and for the frames of the deepest
 * path - and still under a 1 MB main-thread stack as Windows has.
 */
#define SR_SLAB		(64u * 1024u)
#define SR_PAINT	0xA5

/*
 * Depths are counted from the top of the slab, which is where the frame of
 * the library call being examined begins. Deeper is a larger number.
 */
#define SR_AT(depth)	(sr_snap[SR_SLAB - (depth)])

/*
 * The band the wipe covers begins where the entry point's own frame ends: the
 * array is declared one call below that frame and so lands beneath it, which
 * leaves the entry point's own locals standing - the caller's buffer pointer,
 * the length, a return address, the stack canary. None of that is noise
 * source state and none of it is the wipe's to clear.
 *
 * Where that boundary falls is measured, not written down here - see
 * sr_zero_run(). It is a frame size, and frame sizes are precisely what
 * changes under another compiler or target: on x86_64 it is 32 bytes below
 * jent_entropy_init() and 96 below jent_read_entropy(), and a constant
 * generous enough for the second would skip, in the first, a stretch the wipe
 * really does cover and really does have to.
 */

/*
 * Only the fallback for the report when there is no array to find, so that a
 * wipe that never ran is described by where its residue lies rather than by
 * counting the entry frame's own pointers as residue.
 */
#define SR_ENTRY_FRAME_MAX	256u

/*
 * The band below the array, which nothing should have reached at all. It
 * starts clear of the wipe's own frame and of jent_memset_secure()'s, both of
 * which land just under the array and are written after it is cleared - that
 * is the wipe's state, not the path's. Some 140 bytes of it on x86_64.
 */
#define SR_BELOW		512u
#define SR_UNTOUCHED_LEN	4096u

/* How deep the snapshot is searched at all. */
#define SR_SEARCH_BOTTOM	((size_t)JENT_STACK_SCRUB_LEN + SR_BELOW +     \
				 SR_UNTOUCHED_LEN + SR_ENTRY_FRAME_MAX)

/* Implausible as a length, count, pointer or return address. */
#define SR_MAGIC	0x5EC0DE0000000000ULL

/*
 * The magic's four bytes as they sit in memory, either byte order, so the
 * check does not depend on the endianness of the machine.
 */
static const unsigned char sr_sig_le[4] = { 0x00, 0xDE, 0xC0, 0x5E };
static const unsigned char sr_sig_be[4] = { 0x5E, 0xC0, 0xDE, 0x00 };

static uint64_t sr_tick;

/* The step must vary or every measurement is stuck and the loop never ends. */
static void sr_timer(void *arg, uint64_t *out)
{
	(void)arg;

	sr_tick += 0x40000 + (sr_tick % 293);
	*out = SR_MAGIC | (sr_tick & 0xffffffffULL);
}

static volatile unsigned char *sr_painted;
static unsigned char sr_snap[SR_SLAB];

/* The frame is gone on return: the caller then runs the library over it. */
static void sr_paint_below(void)
{
	volatile unsigned char slab[SR_SLAB];
	size_t i;

	for (i = 0; i < SR_SLAB; i++)
		slab[i] = SR_PAINT;

	sr_painted = slab;
}

/*
 * Take the copy the checks below work on. A macro, not a function: a survey
 * that is itself a call puts its own frame on the very addresses it is about
 * to read, and would report its own locals as the library's residue. This has
 * to be the first thing the caller does once the library returns.
 */
#define SR_SNAPSHOT()							       \
	do {								       \
		volatile unsigned char *sl_ = sr_painted;		       \
		size_t i_;						       \
									       \
		for (i_ = 0; i_ < SR_SLAB; i_++)			       \
			sr_snap[i_] = sl_[i_];				       \
	} while (0)

/* Occurrences of @needle between the two depths, inclusive. */
static size_t sr_count(size_t top, size_t bottom,
		       const void *needle, size_t nlen)
{
	const unsigned char *n = needle;
	size_t d, j, found = 0;

	/* A value ending at @d occupies the nlen depths up to and including it. */
	for (d = top + nlen - 1; d <= bottom; d++) {
		for (j = 0; j < nlen; j++) {
			/* The bytes of a value run from deeper to shallower. */
			if (SR_AT(d - j) != n[j])
				break;
		}
		if (j == nlen)
			found++;
	}

	return found;
}

/*
 * The longest run of zero bytes in the snapshot, and where it starts. The
 * wipe writes JENT_STACK_SCRUB_LEN contiguous zeros; nothing else on these
 * paths writes a run anywhere near that long, so the run is the array, and
 * its start is where the entry point's frame ends. A run that begins a few
 * bytes early - the entry frame happening to end in zeros of its own - only
 * moves the examined band up by those bytes, which are zero either way.
 */
static size_t sr_zero_run(size_t *start)
{
	size_t d, run = 0, at = 0, best = 0, best_at = 0;

	for (d = 1; d <= SR_SEARCH_BOTTOM; d++) {
		if (SR_AT(d) != 0x00) {
			run = 0;
			continue;
		}

		if (!run)
			at = d;
		run++;

		if (run > best) {
			best = run;
			best_at = at;
		}
	}

	*start = best_at;
	return best;
}

/* Every timestamp carries the magic, whichever way the target stores it. */
static size_t sr_count_timestamps(size_t top, size_t bottom)
{
	return sr_count(top, bottom, sr_sig_le, sizeof(sr_sig_le)) +
	       sr_count(top, bottom, sr_sig_be, sizeof(sr_sig_be));
}

/*
 * A written zero byte is one the wipe reached; a non-zero one it did not. @lo
 * and @hi bound where the non-zero bytes were found, which is what tells a
 * reader whether the path had merely grown or the wipe had gone missing. All
 * but @written may be NULL, a caller asking only what it goes on to report.
 */
static void sr_survey(size_t top, size_t bottom, size_t *written,
		      size_t *nonzero, size_t *lo, size_t *hi)
{
	size_t d, w = 0, nz = 0, first = 0, last = 0;

	for (d = top; d <= bottom; d++) {
		unsigned char b = SR_AT(d);

		if (b == SR_PAINT)
			continue;
		w++;
		if (b != 0x00) {
			nz++;
			if (!first)
				first = d;
			last = d;
		}
	}

	*written = w;
	if (nonzero)
		*nonzero = nz;
	if (lo)
		*lo = first;
	if (hi)
		*hi = last;
}

/*
 * The checks every entry point gets, on the snapshot the caller has already
 * taken. @what names the claim a failure is reported against.
 */
static void sr_check(const char *what, const char *entry)
{
	size_t run, start = 0, top, bottom;
	size_t written = 0, nonzero = 0, lo = 0, hi = 0;
	size_t beyond = 0, beyond_hi = 0;
	size_t stamps;

	run = sr_zero_run(&start);

	/*
	 * The array, whole. Stated first because the bands below are placed
	 * by what it found, and a wipe that did not run leaves nothing to
	 * place them by.
	 */
	JENT_UT_TRUE(run >= (size_t)JENT_STACK_SCRUB_LEN, what);
	if (run < (size_t)JENT_STACK_SCRUB_LEN)
		start = SR_ENTRY_FRAME_MAX;

	top = start;
	bottom = start + JENT_STACK_SCRUB_LEN - 1;

	sr_survey(top, bottom, &written, &nonzero, &lo, &hi);
	sr_survey(bottom + SR_BELOW, bottom + SR_BELOW + SR_UNTOUCHED_LEN,
		  &beyond, NULL, NULL, &beyond_hi);

	/*
	 * Over everything, the entry point's own frame included. A raw time
	 * stamp is no frame's legitimate local, so unlike a non-zero byte it
	 * needs no allowance made for the frame the wipe cannot reach - and
	 * the startup leaves two of them exactly there, some 90 bytes down,
	 * where a band that started below the entry frame would not look.
	 */
	stamps = sr_count_timestamps(1, SR_SEARCH_BOTTOM);

	printf("  %s: the wipe cleared %zu bytes from %zu down; of the %u it "
	       "covers, %zu were written and %zu left non-zero; %zu bytes "
	       "written below it\n",
	       entry, run, start, JENT_STACK_SCRUB_LEN, written, nonzero,
	       beyond);
	if (nonzero)
		printf("    residue lies %zu to %zu bytes below the entry "
		       "frame\n", lo, hi);
	if (beyond)
		printf("    the path reached %zu bytes below the entry frame, "
		       "past the %u the wipe covers\n",
		       beyond_hi, JENT_STACK_SCRUB_LEN);

	JENT_UT_EQ(nonzero, 0, what);
	JENT_UT_EQ(stamps, 0, what);

	/*
	 * Nothing may have run past the array. If the path has grown deeper
	 * than JENT_STACK_SCRUB_LEN, this is the only check that can say so:
	 * the band above is cleared by the wipe either way.
	 */
	JENT_UT_EQ(beyond, 0, what);
}

/*
 * The startup path, and the first one this program takes: the deepest run of
 * a process is its first jent_entropy_init*(), the one that still has the
 * conditioning known answer tests and the GCD analysis ahead of it. Every
 * later call finds that work done and returns from a shallower frame.
 */
static void test_startup_scrubs_its_stack(void)
{
	static const char *what = "a startup leaves no state on the stack";
	int rc;

	jent_ut_group("the startup path does not leave state on the stack");

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return;
	}

	sr_paint_below();
	rc = jent_entropy_init();
	SR_SNAPSHOT();

	if (rc) {
		JENT_UT_SKIP(what, "the startup does not pass on this machine");
		return;
	}

	sr_check(what, "jent_entropy_init");
}

/*
 * The allocation runs a startup collection of its own, and a health-test
 * reset ladder on top of it where the startup does not pass at once.
 */
static void test_alloc_scrubs_its_stack(void)
{
	static const char *what = "an allocation leaves no state on the stack";
	struct rand_data *ec;

	jent_ut_group("the allocation does not leave state on the stack");

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return;
	}

	if (jent_entropy_init()) {
		JENT_UT_SKIP(what, "the startup does not pass on this machine");
		return;
	}

	sr_paint_below();
	ec = jent_entropy_collector_alloc(0, 0);
	SR_SNAPSHOT();

	if (!ec) {
		JENT_UT_SKIP(what, "no collector");
		return;
	}

	sr_check(what, "jent_entropy_collector_alloc");

	jent_entropy_collector_free(ec);
}

/*
 * A collector on the mock clock, or NULL where there can be none. One on the
 * internal timer is refused: its counting thread would be the time source, so
 * a stamp found on the stack could not be attributed.
 */
static struct rand_data *sr_collector(const char *what)
{
	struct rand_data *ec;

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return NULL;
	}

	if (jent_entropy_init()) {
		JENT_UT_SKIP(what, "the startup does not pass on this machine");
		return NULL;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_SKIP(what, "no collector");
		return NULL;
	}

	if (ec->enable_notime) {
		JENT_UT_SKIP(what,
			     "this collector runs on the internal timer, so "
			     "the mock is not its time source");
		jent_entropy_collector_free(ec);
		return NULL;
	}

	return ec;
}

static void test_generate_scrubs_its_stack(void)
{
	static const char *what = "a generate leaves no state on the stack";
	struct rand_data *ec;
	unsigned char out[32];
	ssize_t rc;

	jent_ut_group("the generation path does not leave state on the stack");

	ec = sr_collector(what);
	if (!ec)
		return;

	sr_paint_below();
	rc = jent_read_entropy(ec, (char *)out, sizeof(out));
	SR_SNAPSHOT();

	if (rc != (ssize_t)sizeof(out)) {
		JENT_UT_SKIP(what, "the generate did not deliver a block");
		jent_entropy_collector_free(ec);
		return;
	}

	sr_check(what, "jent_read_entropy");

	jent_entropy_collector_free(ec);
}

/*
 * Separate from the wipe: the block is not supposed to reach the stack at
 * all, being copied from the conditioning context into the caller's buffer.
 * Searched over the whole slab - no frame here holds a copy to skip.
 */
static void test_generate_leaves_no_output(void)
{
	static const char *what = "a generate leaves no output on the stack";
	struct rand_data *ec;
	unsigned char out[32];
	size_t i, j, whole = 0, half = 0;
	ssize_t rc;

	jent_ut_group("the generate does not leave its output on the stack");

	ec = sr_collector(what);
	if (!ec)
		return;

	sr_paint_below();
	rc = jent_read_entropy(ec, (char *)out, sizeof(out));
	SR_SNAPSHOT();

	for (i = 0; i + sizeof(out) <= SR_SLAB; i++) {
		for (j = 0; j < sizeof(out); j++) {
			if (sr_snap[i + j] != out[j])
				break;
		}
		if (j == sizeof(out))
			whole++;
		/* Half of an output block is still half an output block. */
		if (j >= sizeof(out) / 2)
			half++;
	}

	if (rc != (ssize_t)sizeof(out)) {
		JENT_UT_SKIP(what, "the generate did not deliver a block");
		jent_entropy_collector_free(ec);
		return;
	}

	JENT_UT_EQ(whole, 0, "the returned block is not on the stack");
	JENT_UT_EQ(half, 0, "no half of the returned block is on the stack");

	jent_entropy_collector_free(ec);
}

int main(void)
{
	jent_ut_setup();

	/* First: the deepest startup a process runs is its first one. */
	test_startup_scrubs_its_stack();
	test_alloc_scrubs_its_stack();
	test_generate_scrubs_its_stack();
	test_generate_leaves_no_output();

	return jent_ut_report("unit-stack-residue");
}
