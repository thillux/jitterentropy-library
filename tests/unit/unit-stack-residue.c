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
 * All three read a frame the library has returned from, which only a build
 * that writes nothing there afterwards allows - a sanitizer does - so all
 * three skip where that view is gone. What establishes it is a wipe of the
 * test's own, in the shape of the library's and owing it nothing: if that one
 * can be seen and the library's cannot, the wipe did not run, and the three
 * are reported rather than skipped. See sr_control_observable().
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

/* The deepest byte the run reached: the last one no longer holding paint. */
static size_t sr_reach(void)
{
	size_t d, deepest = 0;

	for (d = 1; d < SR_SLAB; d++) {
		if (SR_AT(d) != SR_PAINT)
			deepest = d;
	}

	return deepest;
}

/*
 * What the four controls below establish, before any entry point is
 * examined.
 */
static int sr_wipe_measurable;
static size_t sr_observable_run;
static size_t sr_control_run;
static size_t sr_alloc_reach;
static size_t sr_random_reach;

/*
 * The deepest excursion an entry point makes outside the noise source: the
 * allocation and the identifier draw, whichever of the two goes further on
 * this machine. See sr_control_alloc() and sr_control_random().
 */
static size_t sr_os_reach(void)
{
	return (sr_random_reach > sr_alloc_reach) ? sr_random_reach :
						    sr_alloc_reach;
}

static const char sr_unmeasurable[] =
	"this build writes into the frame after the wipe returns";

/*
 * The test's own wipe, in the shape of the library's: an array of the same
 * length one call below the caller's frame, and then the scalars that land on
 * the band above it, from the same frame and after it - see
 * jent_stack_scrub() in src/jitterentropy-base.c, which this mirrors down to
 * being a macro, so that the array sits at the same depth below the frame
 * that runs it.
 *
 * Written out here rather than borrowed from the library: this is the probe
 * that decides whether the library's wipe can be seen, and a probe built out
 * of the code it is there to judge cannot tell "I cannot see it" from "it did
 * not happen". That is the whole point of it, so it owes the library nothing.
 *
 * volatile because every store here is dead to any eye but the one that
 * matters - what the frame still holds after the call - which is exactly the
 * kind of store a compiler may drop.
 */
static void sr_own_wipe_array(void)
{
	volatile unsigned char scrub[JENT_STACK_SCRUB_LEN];
	size_t i;

	for (i = 0; i < (size_t)JENT_STACK_SCRUB_LEN; i++)
		scrub[i] = 0x00;
}

static void sr_own_wipe_frame(void)
{
	volatile unsigned long z0 = 0, z1 = 0, z2 = 0, z3 = 0;
	volatile unsigned long z4 = 0, z5 = 0, z6 = 0, z7 = 0;

	(void)z0; (void)z1; (void)z2; (void)z3;
	(void)z4; (void)z5; (void)z6; (void)z7;
}

#define sr_own_wipe()							       \
	do {								       \
		sr_own_wipe_array();					       \
		sr_own_wipe_frame();					       \
	} while (0)

/*
 * Whether a wipe can be seen in this build at all. Every claim here reads the
 * frame once the call has returned, which takes it that nothing writes there
 * between the wipe and the snapshot. A build that instruments every function
 * does: ThreadSanitizer calls __tsan_func_exit() on the way out and that
 * frame lands on what the wipe has just cleared, while AddressSanitizer pads
 * every frame with redzones. A wipe still runs either way. What is gone is
 * the ability to attribute what is found afterwards to the library, so the
 * claims are skipped rather than reported against it.
 *
 * Measured, not deduced from a sanitizer macro: what takes the view away is a
 * build writing into the frame after the wipe, and that is a property to
 * observe rather than a list of build flags to keep up to date.
 *
 * Measured with the test's own wipe, not the library's, which is the whole
 * of the difference between this control and sr_control_wipe(). Asking
 * jent_stack_scrub() whether jent_stack_scrub() can be seen makes the skip
 * fire on a build where the wipe is simply gone - the #if in
 * jitterentropy-base.c taking the freestanding branch on a hosted target, a
 * caller of it dropped, the macro emptied - and every claim this program
 * makes is then skipped and the suite passes. That is the one regression a
 * program named for the residue exists to catch, so it is the one it must not
 * report as an unsupported build.
 */
static void sr_control_observable(void)
{
	size_t start = 0;

	sr_paint_below();
	sr_own_wipe();
	SR_SNAPSHOT();

	sr_observable_run = sr_zero_run(&start);
	sr_wipe_measurable =
		(sr_observable_run >= (size_t)JENT_STACK_SCRUB_LEN);
}

/*
 * What the library's wipe leaves, on the same terms. Read against the control
 * above: nothing here and a full run there is the wipe failing to run,
 * short in both is a build that cannot show it either way.
 *
 * The two do not come out equal even when both run. Under AddressSanitizer
 * the library's wipe measures 4064 here against the 4096 of the test's own,
 * the redzone of the frame that jent_stack_scrub_frame() gets landing 32
 * bytes into the array below it - an artefact of the frame this control calls
 * from, and one the entry points do not show. So what is asked of this number
 * is coarse, and sr_check() is left to hold the wipe to its exact length on
 * the paths where a shortfall is the library's own. See test_the_wipe_runs().
 */
static void sr_control_wipe(void)
{
	size_t start = 0;

	sr_paint_below();
	jent_stack_scrub();
	SR_SNAPSHOT();

	sr_control_run = sr_zero_run(&start);
}

/*
 * The two controls below measure the excursions an entry point makes into the
 * operating system: the allocation, and the identifier draw. Both go far
 * below anything the noise source touches, and neither ever held a time
 * stamp. Only what the noise source reached is the wipe's to cover, so the
 * band examined below the wipe starts below the deeper of the two where that
 * is deeper than the wipe - see sr_os_reach(). Nothing else is relaxed by
 * them: the stamp check still sweeps the whole snapshot, so a time stamp
 * surviving down among these frames is still a failure.
 *
 * Both are one-time costs of a process, paid by whichever call comes first:
 * Windows brings up the machinery behind these calls on first use and every
 * later call stays shallow - 4.3 kB against 552 bytes for the allocation
 * here. So it matters that these run before any entry point does, which is
 * what main() arranges; measured second, each would report the shallow figure
 * and leave the deep one to land on an entry point instead.
 */

/*
 * How deep an allocation alone reaches. Before it samples anything,
 * jent_entropy_collector_alloc() allocates the collector and its memory
 * region, and on Windows that is VirtualAlloc(), VirtualProtect() and
 * VirtualLock() - 4.3 kB there against the 1.9 kB the same entry point needs
 * on Linux.
 *
 * jent_entropy_collector_alloc_internal() is that allocation without the
 * startup ladder on top. It is reached two frames shallower here than the
 * entry point reaches it, which is what the SR_BELOW slack at the band covers.
 * A failure returns NULL, and is still a measurement: the allocations happen
 * before anything that can fail.
 */
static void sr_control_alloc(void)
{
	struct rand_data *ec;
	unsigned int flags;

	(void)jent_set_mock_timer(sr_timer, NULL);
	flags = jent_update_secure_mem(0);

	sr_paint_below();
	ec = jent_entropy_collector_alloc_internal(0, flags);
	SR_SNAPSHOT();

	sr_alloc_reach = sr_reach();

	if (ec)
		jent_entropy_collector_free(ec);
}

/*
 * How deep the identifier draw reaches. jent_entropy_collector_alloc() ends
 * by drawing the per-instance UUID, and those sixteen bytes come from the
 * operating system's CSPRNG rather than from the noise source - see
 * jent_os_random_bytes() in arch/jitterentropy-arch-random.c. On Windows that
 * is BCryptGenRandom(), whose first call in a process stands up the
 * system-preferred provider and descends 6.7 kB: past what the noise source
 * touches, and past the 4096 the wipe covers, so it is what the band below
 * the wipe has to start under there. On Linux the same draw is a getrandom()
 * syscall and reaches nowhere near as far.
 *
 * jent_uuid_generate() rather than the entry point, for the same reason
 * sr_control_alloc() calls the internal allocation: the entry point is what
 * this places the band for, and a control that ran it would have nothing left
 * to measure it against.
 */
static void sr_control_random(void)
{
	char uuid[JENT_UUID_STRLEN];

	sr_paint_below();
	jent_uuid_generate(uuid);
	SR_SNAPSHOT();

	sr_random_reach = sr_reach();
}

/*
 * The checks every entry point gets, on the snapshot the caller has already
 * taken. @what names the claim a failure is reported against.
 */
static void sr_check(const char *what, const char *entry)
{
	size_t run, start = 0, top, bottom, below;
	size_t written = 0, nonzero = 0, lo = 0, hi = 0;
	size_t beyond = 0, beyond_hi = 0;
	size_t stamps;

	if (!sr_wipe_measurable) {
		JENT_UT_SKIP(what, sr_unmeasurable);
		return;
	}

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

	/*
	 * Where the band below the wipe begins: clear of the wipe's own
	 * frames, and clear of the operating system excursions where those
	 * are deeper - see sr_control_alloc() and sr_control_random().
	 */
	below = bottom + SR_BELOW;
	if (sr_os_reach() + SR_BELOW > below)
		below = sr_os_reach() + SR_BELOW;
	if (below + SR_UNTOUCHED_LEN >= SR_SLAB)
		below = SR_SLAB - SR_UNTOUCHED_LEN - 1;

	sr_survey(top, bottom, &written, &nonzero, &lo, &hi);
	sr_survey(below, below + SR_UNTOUCHED_LEN, &beyond, NULL, NULL,
		  &beyond_hi);

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
		       "past the %u the wipe covers and the %zu the operating "
		       "system excursions reach\n",
		       beyond_hi, JENT_STACK_SCRUB_LEN, sr_os_reach());

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
 * That the wipe runs at all, asked of jent_stack_scrub() directly and before
 * any entry point is examined.
 *
 * The claims below all read a frame the library has returned from, which only
 * a build that writes nothing there afterwards allows - so each of them skips
 * where that view is gone. This one draws the line those skips need: a wipe
 * of the test's own, in this same position, was visible, so the library's
 * leaving nothing is the library's answer to give and not the build's.
 *
 * Stated as a claim of its own rather than left to the entry points, which
 * make the same demand exactly and on the paths that matter. Two reasons to
 * say it twice. The entry points report where residue was found, which says
 * nothing useful when the answer is that nothing cleared it - the residue is
 * then simply everything the path wrote. And each of them skips for reasons
 * of its own, the mock time source among them; a build where all three skip
 * leaves nothing at all standing behind the program's name, and this one
 * still stands.
 *
 * Half the length, not the whole of it: this frame is not an entry point, and
 * a build may write into what a wipe called straight from it has cleared -
 * AddressSanitizer takes 32 bytes off the end here and not on any path below.
 * Nothing between the two answers this separates, though. A wipe that ran
 * leaves thousands of contiguous zero bytes and a wipe that did not leaves
 * none, no noise source path writing a zero run remotely this long of its
 * own, so no threshold in between is delicate. The wipe's exact length is
 * sr_check()'s to demand, where a shortfall is attributable.
 */
static void test_the_wipe_runs(void)
{
	static const char *what = "the wipe leaves a run where one can be seen";

	jent_ut_group("the wipe runs where the test can see it");

	if (!sr_wipe_measurable) {
		JENT_UT_SKIP(what, sr_unmeasurable);
		return;
	}

	JENT_UT_TRUE(sr_control_run >= (size_t)JENT_STACK_SCRUB_LEN / 2, what);
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

	/*
	 * The controls first: they establish what this build lets the checks
	 * below see, and what an allocation and an identifier draw cost on
	 * this machine. None of them runs a startup - the deepest run a
	 * process makes is its first jent_entropy_init(), and that one belongs
	 * to the test after them.
	 *
	 * Being first is what the last two are owed: the excursions they
	 * measure are one-time costs of the process, and a control that ran
	 * second would measure the warm call and leave the cold one to be
	 * charged to an entry point.
	 */
	sr_control_observable();
	sr_control_wipe();
	sr_control_alloc();
	sr_control_random();

	printf("the controls: a wipe of the test's own leaves a run of %zu and "
	       "the library's leaves %zu, where %u is written; an allocation "
	       "alone reaches %zu bytes down and an identifier draw %zu\n",
	       sr_observable_run, sr_control_run, JENT_STACK_SCRUB_LEN,
	       sr_alloc_reach, sr_random_reach);
	if (!sr_wipe_measurable)
		printf("  %s, so what is found in that frame afterwards is "
		       "not the library's to answer for\n", sr_unmeasurable);

	test_the_wipe_runs();

	/* First: the deepest startup a process runs is its first one. */
	test_startup_scrubs_its_stack();
	test_alloc_scrubs_its_stack();
	test_generate_scrubs_its_stack();
	test_generate_leaves_no_output();

	return jent_ut_report("unit-stack-residue");
}
