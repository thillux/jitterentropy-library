/*
 * Jitter RNG: a forced internal timer that fails to start
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
 * Forcing the internal timer is a one-way, process-wide decision: once taken,
 * every later collector is steered to the counting thread and a request for
 * the platform clock is refused. It has to be taken on a counting thread that
 * was measured and passed, not on the request alone - taken on the request, a
 * forced startup that then failed (a single CPU, a thread the system refused)
 * left every subsequent allocation of the process failing, on a platform clock
 * that had just been validated.
 *
 * A program of its own, because unit-notime forces the timer successfully
 * early on and stays forced from then on; the decision under test here is the
 * one that must not be taken, which needs a process where it has not been.
 *
 * The counting thread is made to fail by intercepting the thread backend's
 * creation entry, as unit-notime does: the real one is compiled under another
 * name and the name the library calls is defined here.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include "jitterentropy-arch-atomic.c"

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
static int fi_fail_thread_create;

#define jent_notime_thread_create jent_fi_real_thread_create
#include "jitterentropy-arch-thread.c"
#undef jent_notime_thread_create

int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine start_routine,
			      void *arg)
{
	if (fi_fail_thread_create)
		return -EAGAIN;
	return jent_fi_real_thread_create(ctx, start_routine, arg);
}
#else
#include "jitterentropy-arch-thread.c"
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

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
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

static void test_failed_force_decides_nothing(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	struct rand_data *ec;
	int ret;

	jent_ut_group("a forced internal timer that fails to start");

	/*
	 * The platform clock, validated: what the failed attempt below must
	 * not take away. Without it there is nothing to lose here.
	 */
	if (jent_entropy_init()) {
		JENT_UT_SKIP("the forced timer",
			     "no usable platform clock on this machine");
		return;
	}
	if (jent_notime_forced()) {
		JENT_UT_SKIP("the forced timer",
			     "the startup itself fell back to the internal timer");
		return;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	JENT_UT_TRUE(ec != NULL, "a collector on the platform clock is built");
	jent_entropy_collector_free(ec);

	/* The attempt: the counting thread cannot be started. */
	fi_fail_thread_create = 1;
	ret = jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER);
	fi_fail_thread_create = 0;

	JENT_UT_NE(ret, 0, "the forced startup fails when no thread starts");
	JENT_UT_EQ(jent_notime_forced(), 0,
		   "and a startup that failed forces nothing");

	/*
	 * What used to be lost: the default allocation was steered to the
	 * counting thread it had just seen fail, and asking for the platform
	 * clock by name was refused as disabled - every allocation in the
	 * process returned NULL from here on.
	 */
	ec = jent_entropy_collector_alloc(0, 0);
	JENT_UT_TRUE(ec != NULL, "the default allocation still succeeds");
	if (ec)
		JENT_UT_EQ(ec->enable_notime, 0u,
			   "on the platform clock it was validated for");
	jent_entropy_collector_free(ec);

	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	JENT_UT_TRUE(ec != NULL, "as does asking for the platform clock");
	jent_entropy_collector_free(ec);

	/*
	 * And a forced attempt that does start still takes the decision, so
	 * the fix moved it rather than removed it. Skipped where the counting
	 * thread cannot run at all (one CPU), which the first attempt above
	 * already covered.
	 */
	ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_SKIP("a forced timer that starts",
			     "the counting thread cannot run on this machine");
		return;
	}
	JENT_UT_EQ(ec->enable_notime, 1u, "a forced collector runs the timer");
	JENT_UT_EQ(jent_notime_forced(), 1,
		   "and a forced startup that passed forces the process");
	jent_entropy_collector_free(ec);
#else
	jent_ut_group("a forced internal timer that fails to start");
	JENT_UT_SKIP("the forced timer", "not compiled in");
#endif
}

int main(void)
{
	jent_ut_setup();

	test_failed_force_decides_nothing();

	return jent_ut_report("unit-notime-force");
}
