/* Jitter RNG: Health Tests
 *
 * Copyright (C) 2021 - 2026, Joshua E. Hill <josh@keypair.us>
 * Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy-health.h"
#include "jitterentropy-noise.h"

/*
 * The registered callback, and unlike the switch below it is not a latch: two
 * threads may register at the same time, and one may register while another is
 * already generating from a collector that reaches jent_health_failure() and
 * reads it. Whose registration wins is the caller's business - registering
 * from two threads at once names no winner - but the access has to be atomic
 * or it is a data race, and a torn function pointer is one the reader calls.
 *
 * Held as a jent_fnptr, which is what the atomic accessors are typed on, and
 * converted back to its own type before it is called. See
 * arch/jitterentropy-arch-atomic.h for why that is the shape.
 */
static jent_fnptr fips_cb = NULL;

/*
 * Closed once by the initialization and read by every later caller of
 * jent_set_fips_failure_callback(), from whichever thread. Atomic for the
 * reason given in arch/jitterentropy-arch-atomic.h: the value a race could
 * produce is the only one there is, but the access is a data race all the same.
 */
static int jent_health_cb_switch_blocked = 0;

void jent_health_cb_block_switch(void)
{
	jent_atomic_store_int(&jent_health_cb_switch_blocked, 1);
}

int jent_set_fips_failure_callback_internal(jent_fips_failure_cb cb)
{
	if (jent_atomic_load_int(&jent_health_cb_switch_blocked))
		return -EAGAIN;
	jent_atomic_store_fnptr(&fips_cb, (jent_fnptr)cb);
	return 0;
}

/***************************************************************************
 * Lag Predictor Test
 *
 * This test is a vendor-defined conditional test that is designed to detect
 * a known failure mode where the result becomes mostly deterministic
 * Note that (lag_observations & JENT_LAG_MASK) is the index where the next
 * value provided will be stored.
 ***************************************************************************/

#ifdef JENT_HEALTH_LAG_PREDICTOR

/*
 * These cutoffs are configured using an entropy estimate of 1/osr under an
 * alpha=2^(-22) for a window size of 131072. The other health tests use
 * alpha=2^-30, but operate on much smaller window sizes. This larger selection
 * of alpha makes the behavior per-lag-window similar to the APT test.
 *
 * The permanent cutoffs use alpha=2^(-44), i.e. the square of the intermittent
 * alpha. This follows the convention of the RCT and the APT, which derive
 * their permanent cutoff from alpha=2^-60 out of the intermittent alpha=2^-30.
 *
 * The global cutoffs are calculated using the
 * InverseBinomialCDF(n=(JENT_LAG_WINDOW_SIZE-JENT_LAG_HISTORY_SIZE), p=2^(-1/osr); 1-alpha)
 * The local cutoffs are somewhat more complicated: the probability of no run
 * of length r in n trials is (1 - p x) / ((r + 1 - r x) q) * x^-(n+1), where x
 * is the root near 1 of 1 - x + q p^r x^(r+1).
 *
 * We'll proceed using the notation of SP 800-90B Section 6.3.8 (which is
 * developed in Kelsey-McKay-Turan paper "Predictive Models for Min-entropy
 * Estimation".)
 *
 * Here, we set p=2^(-1/osr), seeking a run of successful guesses (r) with
 * probability of less than (1-alpha). That is, it is very very likely
 * (probability 1-alpha) that there is _no_ run of length r in a block of size
 * JENT_LAG_WINDOW_SIZE-JENT_LAG_HISTORY_SIZE.
 *
 * We have to iteratively look for an appropriate value for the cutoff r.
 *
 * tests/health/cutoffs.py computes both tables, and --check compares them
 * against the ones below.
 */
static const unsigned int jent_lag_global_cutoff_lookup[64] =
	{  66443,  93504, 104761, 110875, 114707, 117330, 119237, 120686,
	  121823, 122739, 123493, 124124, 124660, 125120, 125520, 125871,
	  126181, 126457, 126704, 126926, 127128, 127311, 127479, 127632,
	  127773, 127904, 128025, 128137, 128241, 128339, 128430, 128516,
	  128596, 128671, 128743, 128810, 128874, 128934, 128991, 129045,
	  129097, 129146, 129193, 129238, 129280, 129321, 129360, 129398,
	  129434, 129468, 129501, 129533, 129564, 129593, 129621, 129649,
	  129675, 129700, 129725, 129749, 129772, 129794, 129816, 129836 };
static const unsigned int jent_lag_global_cutoff_permanent_lookup[64] =
	{  66876,  93896, 105108, 111188, 114993, 117596, 119486, 120920,
	  122045, 122951, 123696, 124318, 124847, 125301, 125695, 126041,
	  126346, 126617, 126860, 127079, 127276, 127456, 127621, 127771,
	  127910, 128038, 128156, 128266, 128368, 128463, 128552, 128636,
	  128714, 128788, 128858, 128923, 128985, 129044, 129100, 129153,
	  129203, 129251, 129296, 129340, 129382, 129421, 129459, 129496,
	  129530, 129564, 129596, 129627, 129656, 129685, 129713, 129739,
	  129765, 129789, 129813, 129836, 129858, 129880, 129901, 129921 };
static const unsigned int jent_lag_local_cutoff_lookup[64] =
	{   38,   75,  111,  146,  181,  215,  250,  284,  318,  351,
	   385,  419,  452,  485,  518,  551,  584,  617,  650,  683,
	   715,  748,  781,  813,  845,  878,  910,  942,  974, 1007,
	  1039, 1071, 1103, 1135, 1167, 1198, 1230, 1262, 1294, 1325,
	  1357, 1389, 1420, 1452, 1483, 1515, 1546, 1578, 1609, 1640,
	  1672, 1703, 1734, 1766, 1797, 1828, 1859, 1890, 1922, 1953,
	  1984, 2015, 2046, 2077 };
static const unsigned int jent_lag_local_cutoff_permanent_lookup[64] =
	{   60,  119,  177,  234,  291,  347,  404,  460,  516,  571,
	   627,  683,  738,  793,  848,  903,  958, 1013, 1068, 1123,
	  1177, 1232, 1286, 1341, 1395, 1450, 1504, 1558, 1612, 1666,
	  1720, 1774, 1828, 1882, 1936, 1990, 2044, 2098, 2151, 2205,
	  2259, 2312, 2366, 2419, 2473, 2526, 2580, 2633, 2687, 2740,
	  2793, 2846, 2900, 2953, 3006, 3059, 3112, 3166, 3219, 3272,
	  3325, 3378, 3431, 3484 };

static int jent_lag_init(struct rand_data *ec, unsigned int osr)
{
	/* Every rate the tables promise to cover needs an entry. */
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_lag_global_cutoff_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_lag_global_cutoff_permanent_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_lag_local_cutoff_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_lag_local_cutoff_permanent_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);

	/*
	 * Establish the lag global and local cutoffs based on the presumed
	 * entropy rate of 1/osr. A rate outside the tables has none of its
	 * own: taking another rate's would apply cutoffs stricter than the
	 * 1/osr this one claims, which a healthy noise source fails, so the
	 * initialization refuses it and the caller allocates no collector.
	 * Nothing the API accepts arrives here - JENT_MAX_OSR is held to what
	 * the tables cover, which the build assertions above are half of.
	 */
	if (!osr || osr > JENT_ARRAY_SIZE(jent_lag_global_cutoff_lookup) ||
	    osr > JENT_ARRAY_SIZE(jent_lag_local_cutoff_lookup))
		return 1;

	ec->lag_global_cutoff = jent_lag_global_cutoff_lookup[osr - 1];
	ec->lag_global_cutoff_permanent =
		jent_lag_global_cutoff_permanent_lookup[osr - 1];
	ec->lag_local_cutoff = jent_lag_local_cutoff_lookup[osr - 1];
	ec->lag_local_cutoff_permanent =
		jent_lag_local_cutoff_permanent_lookup[osr - 1];

	return 0;
}

/**
 * Reset the lag counters
 *
 * @param[in] ec Reference to entropy collector
 */
static void jent_lag_reset(struct rand_data *ec)
{
	unsigned int i;

	/* Reset Lag counters */
	ec->lag_prediction_success_count = 0;
	ec->lag_prediction_success_run = 0;
	ec->lag_best_predictor = 0; /* The first guess is basically arbitrary. */
	ec->lag_observations = 0;

	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		ec->lag_scoreboard[i] = 0;
		ec->lag_delta_history[i] = 0;
	}
}

/*
 * A macro for accessing the history. Index 0 is the last observed symbol
 * index 1 is the symbol observed two inputs ago, etc.
 */
#define JENT_LAG_HISTORY(EC,LOC)					       \
	((EC)->lag_delta_history[((EC)->lag_observations - (LOC) - 1) &	       \
	 JENT_LAG_MASK])

/**
 * Insert a new entropy event into the lag predictor test
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta Current time delta
 */
static void jent_lag_insert(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t prediction;
	unsigned int i;

	/* Initialize the delta_history */
	if (ec->lag_observations < JENT_LAG_HISTORY_SIZE) {
		ec->lag_delta_history[ec->lag_observations] = current_delta;
		ec->lag_observations++;
		return;
	}

	/*
	 * The history is initialized. First make a guess and examine the
	 * results.
	 */
	prediction = JENT_LAG_HISTORY(ec, ec->lag_best_predictor);

	if (prediction == current_delta) {
		/* The prediction was correct. */
		ec->lag_prediction_success_count++;
		ec->lag_prediction_success_run++;

		if ((ec->lag_prediction_success_run >=
		     ec->lag_local_cutoff_permanent) ||
		    (ec->lag_prediction_success_count >=
		     ec->lag_global_cutoff_permanent))
			ec->health_failure |= JENT_LAG_FAILURE_PERMANENT;
		else if ((ec->lag_prediction_success_run >=
			  ec->lag_local_cutoff) ||
			 (ec->lag_prediction_success_count >=
			  ec->lag_global_cutoff))
			ec->health_failure |= JENT_LAG_FAILURE;
	} else {
		/* The prediction wasn't correct. End any run of successes.*/
		ec->lag_prediction_success_run = 0;
	}

	/* Now update the predictors using the current data. */
	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		if (JENT_LAG_HISTORY(ec, i) == current_delta) {
			/*
			 * The ith predictor (which guesses i + 1 symbols in
			 * the past) successfully guessed.
			 */
			ec->lag_scoreboard[i] ++;

			/*
			 * Keep track of the best predictor (tie goes to the
			 * shortest lag)
			 */
			if (ec->lag_scoreboard[i] >
			    ec->lag_scoreboard[ec->lag_best_predictor])
				ec->lag_best_predictor = i;
		}
	}

	/*
	 * Finally, update the lag_delta_history array with the newly input
	 * value.
	 */
	ec->lag_delta_history[(ec->lag_observations) & JENT_LAG_MASK] =
								current_delta;
	ec->lag_observations++;

	/*
	 * lag_best_predictor now is the index of the predictor with the largest
	 * number of correct guesses.
	 * This establishes our next guess.
	 */

	/* Do we now need a new window? */
	if (ec->lag_observations >= JENT_LAG_WINDOW_SIZE)
		jent_lag_reset(ec);
}

static inline uint64_t jent_delta2(struct rand_data *ec, uint64_t current_delta)
{
	/* Note that delta2_n = delta_n - delta_{n-1} */
	return jent_delta(JENT_LAG_HISTORY(ec, 0), current_delta);
}

static inline uint64_t jent_delta3(struct rand_data *ec, uint64_t delta2)
{
	/*
	 * Note that delta3_n = delta2_n - delta2_{n-1}
	 *		      = delta2_n - (delta_{n-1} - delta_{n-2})
	 */
	return jent_delta(jent_delta(JENT_LAG_HISTORY(ec, 1),
				     JENT_LAG_HISTORY(ec, 0)), delta2);
}

void jent_lag_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	unsigned int i;

	new_ec->lag_prediction_success_run = old_ec->lag_prediction_success_run;
	new_ec->lag_prediction_success_count =
		old_ec->lag_prediction_success_count;
	new_ec->lag_best_predictor = old_ec->lag_best_predictor;
	new_ec->lag_observations = old_ec->lag_observations;

	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		new_ec->lag_scoreboard[i] = old_ec->lag_scoreboard[i];
		new_ec->lag_delta_history[i] = old_ec->lag_delta_history[i];
	}
}

#else /* JENT_HEALTH_LAG_PREDICTOR */

static inline void jent_lag_insert(struct rand_data *ec, uint64_t current_delta)
{
	(void)ec;
	(void)current_delta;
}

static inline uint64_t jent_delta2(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t delta2 = jent_delta(ec->last_delta, current_delta);

	ec->last_delta = current_delta;
	return delta2;
}

static inline uint64_t jent_delta3(struct rand_data *ec, uint64_t delta2)
{
	uint64_t delta3 = jent_delta(ec->last_delta2, delta2);

	ec->last_delta2 = delta2;
	return delta3;
}

static inline int jent_lag_init(struct rand_data *ec, unsigned int osr)
{
	(void)ec;
	(void)osr;

	return 0;
}

void jent_lag_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	new_ec->last_delta = old_ec->last_delta;
	new_ec->last_delta2 = old_ec->last_delta2;
}

#endif /* JENT_HEALTH_LAG_PREDICTOR */

/***************************************************************************
 * Adaptive Proportion Test
 *
 * This test complies with SP800-90B section 4.4.2.
 ***************************************************************************/

/*
 * See the SP 800-90B comment #10b for the corrected cutoff for the SP 800-90B
 * APT.
 * http://www.untruth.org/~josh/sp80090b/UL%20SP800-90B-final%20comments%20v1.9%2020191212.pdf
 * In in the syntax of R, this is C = 2 + qbinom(1 - 2^(-30), 511, 2^(-1/osr)).
 * (The original formula wasn't correct because the first symbol must
 * necessarily have been observed, so there is no chance of observing 0 of these
 * symbols.)
 *
 * For the alpha < 2^-53, R cannot be used as it uses a float data type without
 * arbitrary precision. tests/health/cutoffs.py computes these tables with
 * mpmath, and --check compares them against the ones below.
 *
 * From osr 15 on this yields the maximal allowable value of 512 (by FIPS 140-2
 * IG 7.19 Resolution # 16, we cannot choose a cutoff value that renders the
 * test unable to fail). The tables cover osr 1 to
 * JENT_HEALTH_CUTOFF_TABLE_OSR; the build assertion below keeps them doing
 * so.
 */
static const unsigned int jent_apt_cutoff_lookup[64] =
	{ 325, 422, 459, 477, 488, 494, 499, 502, 505, 507, 508, 509,
	  510, 511, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512 };
static const unsigned int jent_apt_cutoff_permanent_lookup[64] =
	{ 355, 447, 479, 494, 502, 507, 510, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512, 512,
	  512, 512, 512, 512 };

static int jent_apt_init(struct rand_data *ec)
{
	/* Every rate the tables promise to cover needs an entry. */
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_apt_cutoff_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_apt_cutoff_permanent_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);

	/*
	 * Establish the apt_cutoff based on the presumed entropy rate of
	 * 1/osr. A rate outside the table is refused, for the reason
	 * jent_lag_init() states.
	 */
	if (!ec->osr || ec->osr > JENT_ARRAY_SIZE(jent_apt_cutoff_lookup))
		return 1;

	ec->apt_cutoff = jent_apt_cutoff_lookup[ec->osr - 1];
	ec->apt_cutoff_permanent =
		jent_apt_cutoff_permanent_lookup[ec->osr - 1];

	return 0;
}

/*
 * For NTG.1: 8-fold security margin
 * alpha for intermdiate error: 2^-30
 * alpha for permanent error: 2^-60
 *
 * Example formula for R for intermediate cutoffs:
 * C = 2 + qbinom(1 - 2^(-30), 511, 2^(-8/osr))
 */
static const unsigned int jent_apt_cutoff_lookup_ntg1[64] =
	{  17,  71, 136, 191, 236, 272, 301, 325, 345, 361, 375, 388,
	  398, 407, 415, 422, 429, 434, 439, 444, 448, 452, 455, 459,
	  462, 464, 467, 469, 471, 473, 475, 477, 478, 480, 481, 483,
	  484, 485, 486, 488, 489, 490, 490, 491, 492, 493, 494, 494,
	  495, 496, 496, 497, 498, 498, 499, 499, 500, 500, 500, 501,
	  501, 502, 502, 502 };
static const unsigned int jent_apt_cutoff_permanent_lookup_ntg1[64] =
	{  26,  92, 162, 221, 267, 303, 332, 355, 375, 390, 404, 415,
	  425, 433, 440, 447, 453, 458, 462, 466, 470, 473, 476, 479,
	  481, 484, 486, 487, 489, 491, 492, 494, 495, 496, 497, 498,
	  499, 500, 501, 502, 503, 503, 504, 505, 505, 506, 506, 507,
	  507, 508, 508, 509, 509, 509, 510, 510, 510, 510, 511, 511,
	  511, 511, 512, 512 };

static int jent_apt_init_ntg1(struct rand_data *ec)
{
	/* Every rate the tables promise to cover needs an entry. */
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_apt_cutoff_lookup_ntg1) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_apt_cutoff_permanent_lookup_ntg1) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);

	if (!ec->osr || ec->osr > JENT_ARRAY_SIZE(jent_apt_cutoff_lookup_ntg1))
		return 1;

	ec->apt_cutoff = jent_apt_cutoff_lookup_ntg1[ec->osr - 1];
	ec->apt_cutoff_permanent =
		jent_apt_cutoff_permanent_lookup_ntg1[ec->osr - 1];

	return 0;
}

static void jent_apt_reinit(struct rand_data *ec,
			    uint64_t current_delta,
			    unsigned int apt_count,
			    unsigned int apt_observations)
{
	ec->apt_base = current_delta;	/* APT Step 1 */
	ec->apt_base_set = 1;		/* APT Step 2 */

	/*
	 * Reset APT counter. Both callers state the count they want: one for
	 * the window a first symbol opens, the priming of a reallocation for
	 * a window that continues.
	 */
	ec->apt_count = apt_count;
	ec->apt_observations = apt_observations;
}

void jent_apt_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	unsigned int primed;

	if (!old_ec->apt_observations)
		return;

	/*
	 * APT re-initialization to intermittent error: prime the replacement
	 * at its own intermittent cutoff, so a window that is continuing to
	 * fail escalates to the permanent failure instead of restarting from
	 * zero.
	 *
	 * Only where that leaves the permanent cutoff out of reach of a
	 * single further repeat, though. Both cutoffs are capped at the
	 * window size and the common tables meet there: at osr 14 they are
	 * one apart, from osr 15 on both are JENT_APT_WINDOW_SIZE. Priming at
	 * the intermittent cutoff there would turn the very next delta equal
	 * to the base symbol into a permanent APT failure - which ends the
	 * instance for good, and panics a fips=1 kernel - on evidence a
	 * collector that had not been reallocated would need a whole window
	 * of identical symbols to reach. Note the reallocation happens for
	 * any health test that failed, so the APT need not be the test that
	 * triggered it.
	 *
	 * Where the two have met, the replacement continues from the count
	 * the old window actually reached. That is the evidence the test has,
	 * so the reallocation still cannot be used to clear it.
	 */
	primed = new_ec->apt_cutoff;
	if (primed + 1 >= new_ec->apt_cutoff_permanent)
		primed = old_ec->apt_count;

	/* Whatever the tables hold, the priming itself never fails the test. */
	if (primed >= new_ec->apt_cutoff_permanent)
		primed = new_ec->apt_cutoff_permanent - 1;

	jent_apt_reinit(new_ec, old_ec->apt_base, primed,
			old_ec->apt_observations);
}

/**
 * Reset the APT counter
 *
 * @param[in] ec Reference to entropy collector
 */
static void jent_apt_reset(struct rand_data *ec)
{
	/* When reset, accept the _next_ value input as the new base. */
	ec->apt_base_set = 0;
}

/**
 * Insert a new entropy event into APT
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta  Current time delta
 */
static void jent_apt_insert(struct rand_data *ec, uint64_t current_delta)
{
	current_delta &= JENT_APT_MASK;

	/* Initialize the base reference */
	if (!ec->apt_base_set) {
		jent_apt_reinit(ec, current_delta, 1, 1);
		return;
	}

	if (current_delta == ec->apt_base) {
		ec->apt_count++;		/* B = B + 1 */

		/* Note, ec->apt_count starts with one. */
		if (ec->apt_count >= ec->apt_cutoff_permanent)
			ec->health_failure |= JENT_APT_FAILURE_PERMANENT;
		else if (ec->apt_count == ec->apt_cutoff)
			ec->health_failure |= JENT_APT_FAILURE;
	}

	ec->apt_observations++;

	/* Completed one window, the next symbol input will be new apt_base. */
	if (ec->apt_observations >= JENT_APT_WINDOW_SIZE)
		jent_apt_reset(ec);		/* APT Step 4 */
}

/***************************************************************************
 * Stuck Test and its use as Repetition Count with Memory Test
 *
 * The Jitter RNG applies the stuck test to the repetition count test with
 * memory as defined in a paper "Überlegungen zum Jitter-RNG" by Jonas Fiege,
 * Johannes Mittmann, Werner Schindler, 6. Februar 2026. This document
 * outlines a health test which applies the standard normal distribution. Using
 * the distribution, the cutoffs are calculated by applying the heuristic
 * entropy value (potentially adjusted by a safety factor).
 *
 * The test is defined to cover the window required non-rejected time deltas
 * to be generated for one output block of 256 bits of data. This window
 * is 321 * OSR as implemented by jent_random_data_one, of which tau = 3
 * leaves n = 107 * OSR observations.
 *
 * With p = 2^(1 - safety_factor/OSR), twice the 2^(-H) of the heuristic
 * entropy H = safety_factor/OSR, and p' = min(p, 1/2), which holds the
 * variance at its maximum once p passes one half, both cutoffs are
 *
 *   floor(n*p + tau * sqrt(n * p' * (1 - p')))
 *
 * capped at n, and at n + 1 for the permanent one. tau is 4 for the
 * intermittent and 5 for the permanent cutoff - the significance levels
 * pnorm(-4) and pnorm(-5) named at the NTG.1 tables below - and safety_factor
 * is 1 for the common case and 8 for NTG.1.
 *
 * In the common case p >= 1 at every OSR, so both cutoffs are the cap and the
 * test cannot fail: that is what disables it there.
 *
 * tests/health/cutoffs.py computes all four tables from that formula, and
 * --check compares them against the ones below.
 ***************************************************************************/

/*
 * Recovery loop count defining the number of successful generation of
 * random blocks after a RCT with mem health alarm to recover from that
 * alarm.
 */
#define JENT_RCT_MEM_RECOVERY_LOOP_CNT 10

/* RCT with memory, safety factor 1, tau = 4: the cap, so no failure. */
static const unsigned short jent_rct_mem_cutoff_lookup[64] =
	{  107,  214,  321,  428,  535,  642,  749,  856,  963, 1070,
	  1177, 1284, 1391, 1498, 1605, 1712, 1819, 1926, 2033, 2140,
	  2247, 2354, 2461, 2568, 2675, 2782, 2889, 2996, 3103, 3210,
	  3317, 3424, 3531, 3638, 3745, 3852, 3959, 4066, 4173, 4280,
	  4387, 4494, 4601, 4708, 4815, 4922, 5029, 5136, 5243, 5350,
	  5457, 5564, 5671, 5778, 5885, 5992, 6099, 6206, 6313, 6420,
	  6527, 6634, 6741, 6848 };
/* RCT with memory, safety factor 1, tau = 5: the cap, so no failure. */
static const unsigned short jent_rct_mem_cutoff_permanent_lookup[64] =
	{  108,  215,  322,  429,  536,  643,  750,  857,  964, 1071,
	  1178, 1285, 1392, 1499, 1606, 1713, 1820, 1927, 2034, 2141,
	  2248, 2355, 2462, 2569, 2676, 2783, 2890, 2997, 3104, 3211,
	  3318, 3425, 3532, 3639, 3746, 3853, 3960, 4067, 4174, 4281,
	  4388, 4495, 4602, 4709, 4816, 4923, 5030, 5137, 5244, 5351,
	  5458, 5565, 5672, 5779, 5886, 5993, 6100, 6207, 6314, 6421,
	  6528, 6635, 6742, 6849 };

static int jent_rct_mem_init(struct rand_data *ec)
{
	/* Every rate the tables promise to cover needs an entry. */
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_rct_mem_cutoff_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_rct_mem_cutoff_permanent_lookup) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);

	if (!ec->osr || ec->osr > JENT_ARRAY_SIZE(jent_rct_mem_cutoff_lookup))
		return 1;

	ec->rct_mem_cutoff = jent_rct_mem_cutoff_lookup[ec->osr - 1];
	ec->rct_mem_cutoff_permanent =
		jent_rct_mem_cutoff_permanent_lookup[ec->osr - 1];

	return 0;
}

/*
 * For NTG.1: 8-fold security margin with tau = 4, a significance level of
 * pnorm(-4) yielding 3.17e-05 (roughly 2^-15) for first-order errors. Due to
 * the recovery loop we can afford such higher value.
 */
static const unsigned short jent_rct_mem_cutoff_lookup_ntg1[64] =
	{    4,   46,  134,  255,  399,  560,  733,  856,  963, 1070,
	  1177, 1284, 1391, 1498, 1605, 1712, 1819, 1926, 2033, 2140,
	  2247, 2354, 2461, 2568, 2675, 2782, 2889, 2996, 3103, 3210,
	  3317, 3424, 3531, 3638, 3745, 3852, 3959, 4066, 4173, 4280,
	  4387, 4494, 4601, 4708, 4815, 4922, 5029, 5136, 5243, 5350,
	  5457, 5564, 5671, 5778, 5885, 5992, 6099, 6206, 6313, 6420,
	  6527, 6634, 6741, 6848 };
/*
 * For NTG.1: 8-fold security margin with tau = 5, a significance level of
 * pnorm(-5) yielding about 2^-20.
 */
static const unsigned short jent_rct_mem_cutoff_permanent_lookup_ntg1[64] =
	{    5,   50,  142,  265,  410,  572,  746,  857,  964, 1071,
	  1178, 1285, 1392, 1499, 1606, 1713, 1820, 1927, 2034, 2141,
	  2248, 2355, 2462, 2569, 2676, 2783, 2890, 2997, 3104, 3211,
	  3318, 3425, 3532, 3639, 3746, 3853, 3960, 4067, 4174, 4281,
	  4388, 4495, 4602, 4709, 4816, 4923, 5030, 5137, 5244, 5351,
	  5458, 5565, 5672, 5779, 5886, 5993, 6100, 6207, 6314, 6421,
	  6528, 6635, 6742, 6849 };
static int jent_rct_mem_init_ntg1(struct rand_data *ec)
{
	/* Every rate the tables promise to cover needs an entry. */
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_rct_mem_cutoff_lookup_ntg1) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(jent_rct_mem_cutoff_permanent_lookup_ntg1) <
			  JENT_HEALTH_CUTOFF_TABLE_OSR);

	if (!ec->osr ||
	    ec->osr > JENT_ARRAY_SIZE(jent_rct_mem_cutoff_lookup_ntg1))
		return 1;

	ec->rct_mem_cutoff = jent_rct_mem_cutoff_lookup_ntg1[ec->osr - 1];
	ec->rct_mem_cutoff_permanent =
		jent_rct_mem_cutoff_permanent_lookup_ntg1[ec->osr - 1];

	return 0;
}

static void jent_rct_mem_insert(struct rand_data *ec, unsigned int stuck)
{
	/* Start of a new window */
	if (ec->rct_mem_ctr == 0)
		ec->rct_mem_count = 0;

	/*
	 * If we are outside of a window, do not bother any more: the health
	 * test will not be applied any more.
	 *
	 * We could simply return at this point if we leave the window, but then
	 * we have a multi-modal behavior of the noise source, because if the
	 * window is completed, a significant part of the code is not executed.
	 * Therefore we apply a different strategy: always apply the health
	 * test, but only set a health error if we are within the window.
	 */
#define JENT_RCT_MEM_IN_WINDOW	(ec->rct_mem_ctr < ec->rct_mem_nosr)

	/*
	 * According to the specification of this health test, we only consider
	 * every third iteration count.
	 *
	 * Again, we could simply return here, but that would again imply a
	 * multi-modal behavior. Therefore, always perform the health test
	 * and turn it into a noop.
	 */
#define JENT_RCT_MEM_SKIP_STUCK (ec->rct_mem_ctr % 3)

	/*
	 * We have a stuck value, count it
	 */
	if (stuck && JENT_RCT_MEM_IN_WINDOW && !JENT_RCT_MEM_SKIP_STUCK)
		ec->rct_mem_count++;

	/*
	 * Apply the cut off value.
	 */
	if (ec->rct_mem_count >= ec->rct_mem_cutoff_permanent) {
		if (JENT_RCT_MEM_IN_WINDOW)
			ec->health_failure |= JENT_RCT_MEM_FAILURE_PERMANENT;
	} else if (ec->rct_mem_count == ec->rct_mem_cutoff) {
		/*
		 * This is a "recovery loop" to recover from expected false
		 * positives of the health test. Note, the health test cutoffs
		 * have a significantly higher false positive rate than the
		 * APT and RCT. Therefore, this loop generates additional
		 * random values to see whether no health test is detected
		 * during this loop. If no health test error is seen, then
		 * we incurred an expected spurious false positive that we
		 * can ignore. The random data generated by that recovery
		 * loop is simply added to the internal state and thus is not
		 * wasted.
		 */
		if (!ec->in_recovery) {
			/*
			 * Each recovery block is a window of its own and
			 * resets these, which belong to the outer window
			 * still in progress. Left clobbered, ->rct_mem_ctr
			 * ended at the last block's ->rct_mem_nosr, so
			 * JENT_RCT_MEM_IN_WINDOW was false for the rest of
			 * the outer block: the recovery switched the test off
			 * for the block it was recovering.
			 */
			unsigned short saved_ctr = ec->rct_mem_ctr;
			unsigned short saved_nosr = ec->rct_mem_nosr;

			ec->in_recovery = 1;
			jent_random_data_recovery(
				ec, JENT_RCT_MEM_RECOVERY_LOOP_CNT);
			ec->in_recovery = 0;

			ec->rct_mem_ctr = saved_ctr;
			ec->rct_mem_nosr = saved_nosr;

			/*
			 * Fresh count for the rest of the outer window: the
			 * crossing is forgiven when the blocks above raised
			 * no failure of their own. Any they did raise stands.
			 */
			ec->rct_mem_count = 0;
		} else {
			if (JENT_RCT_MEM_IN_WINDOW)
				ec->health_failure |= JENT_RCT_MEM_FAILURE;
		}
	}

	/*
	 * Count up the seen measurements if we are in the window - this
	 * prevents also a wrap-around.
	 */
	if (JENT_RCT_MEM_IN_WINDOW)
		ec->rct_mem_ctr++;
}

void jent_rct_mem_duplicate(struct rand_data *new_ec)
{
	/*
	 * RCT with memory re-initialization to intermittent error, at the
	 * cutoff of the replacement rather than of the collector it replaces:
	 * the two differ, the replacement being allocated at a higher
	 * oversampling rate, and it is the replacement's own comparisons the
	 * primed value has to be meaningful for. jent_rct_duplicate() states
	 * the same for the RCT, where a mismatched value is outright harmful.
	 *
	 * NOTE: this priming is currently ineffective. Every output block
	 * starts with jent_random_data_one() setting rct_mem_ctr = 0, and the
	 * first jent_rct_mem_insert() of a window then clears rct_mem_count
	 * before any cutoff comparison sees it - so, unlike the RCT/APT/lag
	 * duplication, no escalation state actually survives the reset. Making
	 * it effective would change the health-test semantics (the window-
	 * start reset in jent_rct_mem_insert() would need to spare a primed
	 * value once).
	 */
	new_ec->rct_mem_count = new_ec->rct_mem_cutoff;
}

/***************************************************************************
 * Stuck Test and its use as Repetition Count Test
 *
 * The Jitter RNG uses an enhanced version of the Repetition Count Test
 * (RCT) specified in SP800-90B section 4.4.1. Instead of counting identical
 * back-to-back values, the input to the RCT is the counting of the stuck
 * values during the generation of one Jitter RNG output block.
 *
 * The RCT is applied with an alpha of 2^{-30} compliant to SP800-90B section
 * 4.2 for the intermittent failure and 2^{-60} for permanent failures.
 *
 * During the counting operation, the Jitter RNG always calculates the RCT
 * cut-off value of C. If that value exceeds the allowed cut-off value,
 * the Jitter RNG output block will be calculated completely but discarded at
 * the end. The caller of the Jitter RNG is informed with an error code.
 ***************************************************************************/
static void jent_rct_init(struct rand_data *ec, unsigned short safety)
{
	unsigned short osr = (unsigned short)ec->osr;

	/*
	 * The RCT states its cutoffs as a multiple of the oversampling rate
	 * rather than from a table, so what bounds them is the width of the
	 * counters they are kept in. Asserted for the highest rate the
	 * library accepts, that being a compile-time tunable.
	 */
	JENT_BUILD_BUG_ON(JENT_HEALTH_RCT_PERMANENT_CUTOFF(JENT_MAX_OSR) >
			  USHRT_MAX);

	ec->rct_cutoff = JENT_HEALTH_RCT_INTERMITTENT_CUTOFF(osr);
	ec->rct_cutoff_permanent = JENT_HEALTH_RCT_PERMANENT_CUTOFF(osr);

	if (safety) {
		ec->rct_cutoff = (unsigned short)
			((ec->rct_cutoff + safety - 1) / safety);
		ec->rct_cutoff_permanent = (unsigned short)
			((ec->rct_cutoff_permanent + safety - 1) / safety);
	}
}

void jent_rct_duplicate(struct rand_data *new_ec)
{
	/*
	 * RCT re-initialization to intermittent error: prime the new RCT with
	 * the intermittent cutoff established by jent_rct_init() for the new
	 * collector, so a continuing streak of stuck values escalates to the
	 * permanent error instead of restarting from zero.
	 *
	 * Use the collector's rct_cutoff member instead of recomputing it via
	 * JENT_HEALTH_RCT_INTERMITTENT_CUTOFF: in NTG.1 mode the cutoffs carry
	 * the 8-fold safety divisor, and the undivided value would already
	 * exceed rct_cutoff_permanent, turning the first stuck measurement
	 * after the reallocation into a spurious permanent failure.
	 */
	new_ec->rct_count = (unsigned int)new_ec->rct_cutoff;
}

/**
 * Carry the health test state of a collector over to its replacement
 *
 * The reallocation must not become a way to clear the tests, so the
 * replacement starts primed at the intermittent cutoffs.
 *
 * The RCT and the RCT with memory carry nothing the old clock produced: the
 * duplication only primes their counters at a cutoff, and a non-stuck
 * measurement of the new source clears the priming again, so it can make the
 * test stricter but never weaker. The APT base and the lag history are delta
 * values of the old clock, which outside a compliance mode the replacement
 * need not be reading: carrying them would leave the APT counting repeats of a
 * symbol the new source does not produce, for a whole window. Those two
 * therefore start on the new source's own measurements.
 *
 * @param[in] new_ec The replacement, already allocated and health-initialized
 * @param[in] old_ec The collector it replaces
 */
void jent_health_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	jent_rct_duplicate(new_ec);
	jent_rct_mem_duplicate(new_ec);

	/* A different clock: the two below describe another source. */
	if (new_ec->enable_notime != old_ec->enable_notime)
		return;

	jent_apt_duplicate(new_ec, old_ec);
	jent_lag_duplicate(new_ec, old_ec);
}

/**
 * Repetition Count Test as defined in SP800-90B section 4.4.1
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] stuck Indicator whether the value is stuck
 */
static void jent_rct_insert(struct rand_data *ec, unsigned int stuck)
{
	if (stuck) {
		ec->rct_count++;

		if (ec->rct_count >= ec->rct_cutoff_permanent) {
			ec->health_failure |= JENT_RCT_FAILURE_PERMANENT;
		} else if (ec->rct_count == ec->rct_cutoff) {
			ec->health_failure |= JENT_RCT_FAILURE;
		}
	} else {
		/* Must start at zero to reach the correct cutoff value */
		ec->rct_count = 0;
	}
}

/**
 * Stuck test by checking the:
 * 	1st derivative of the jitter measurement (time delta)
 * 	2nd derivative of the jitter measurement (delta of time deltas)
 * 	3rd derivative of the jitter measurement (delta of delta of time deltas)
 *
 * All values must always be non-zero.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta  Jitter time delta
 *
 * @return
 * 	0 jitter measurement not stuck (good bit)
 * 	1 jitter measurement stuck (reject bit)
 */
unsigned int jent_stuck(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t delta2 = jent_delta2(ec, current_delta);
	uint64_t delta3 = jent_delta3(ec, delta2);
	unsigned int stuck = !current_delta || !delta2 || !delta3;

	/*
	 * Insert the result of the comparison of two back-to-back time
	 * deltas.
	 */
	jent_apt_insert(ec, current_delta);
	jent_lag_insert(ec, current_delta);

	/* RCT with stuck result */
	jent_rct_insert(ec, stuck);
	jent_rct_mem_insert(ec, stuck);

	return stuck;
}

/**
 * Insert an externally obtained time stamp into the health tests
 *
 * The delta against the previously inserted stamp is formed exactly as the
 * noise source forms it - the division by the common timer divisor included -
 * and every health test is run on it; the verdict is read back with
 * jent_health_failure() as usual. Same code, so the same verdict the noise
 * source would have reached. See jitterentropy-health.h for what for.
 *
 * The first stamp of a sequence only primes the reference and is reported as
 * stuck, as the first measurement of a collector is.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] timestamp Externally obtained time stamp
 *
 * @return Whether the resulting measurement is stuck
 */
unsigned int jent_health_insert_timestamp(struct rand_data *ec,
					  uint64_t timestamp)
{
	/*
	 * jent_entropy_collector_alloc() never leaves the divisor at zero - it
	 * substitutes one when no common divisor was established - but a
	 * caller that assembled the collector itself, as the induced failure
	 * tests do, can. Substitute here too rather than dividing by it.
	 */
	uint64_t gcd = ec->jent_common_timer_gcd ?
		       ec->jent_common_timer_gcd : 1;
	uint64_t current_delta = jent_udiv64(jent_delta(ec->prev_time,
							timestamp), gcd);

	ec->prev_time = timestamp;

	return jent_stuck(ec, current_delta);
}

/**
 * Report any health test failures
 *
 * The health tests judge the noise source, and they only report in FIPS
 * mode. A failed conditioning self test (jent_selftest()) is deliberately
 * not part of this bitmask: it judges the conditioning implementation and
 * has to stop the output in every mode, so jent_read_entropy() checks the
 * collector's selftest_failed word directly instead of going through here.
 *
 * @param[in] ec Reference to entropy collector
 *
 * @return a bitmask indicating which tests failed
 *	0 No health test failure
 *	1 RCT failure
 *	2 APT failure
 *	4 Lag predictor test failure
 *	8 RCT with memory failure
 *	1<<JENT_PERMANENT_FAILURE_SHIFT RCT permanent failure
 *	2<<JENT_PERMANENT_FAILURE_SHIFT APT permanent failure
 *	4<<JENT_PERMANENT_FAILURE_SHIFT Lag predictor test permanent failure
 *	8<<JENT_PERMANENT_FAILURE_SHIFT RCT with memory permanent failure
 */
unsigned int jent_health_failure(struct rand_data *ec)
{
	jent_fips_failure_cb cb;

	/* Test is only enabled in FIPS mode */
	if (!ec->is_fips_enabled)
		return 0;

	/*
	 * Read once, so that the call cannot be made on a pointer that was
	 * replaced between the test and it.
	 */
	cb = (jent_fips_failure_cb)jent_atomic_load_fnptr(&fips_cb);

	if (cb && ec->health_failure) {
		cb(ec, ec->health_failure);
	}

	return ec->health_failure;
}

/**
 * Initialize the health tests
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] inittype Startup type
 *
 * @return 0 on success, nonzero if the oversampling rate of @ec is one the
 *	   cutoff tables do not cover - the tests have no cutoffs for it and
 *	   the collector must not be used. The allocation refuses such a rate
 *	   before it gets here, so this is the assertion behind that, not a
 *	   condition a caller of the API can produce.
 */
int jent_health_init(struct rand_data *ec, enum jent_health_init_type inittype)
{
	/* Must start at zero to reach the correct cutoff value */
	ec->rct_count = 0;

	if (jent_lag_init(ec, ec->osr))
		return 1;

	switch (inittype) {
	case jent_health_init_type_ntg1:
		if (jent_apt_init_ntg1(ec))
			return 1;
		jent_rct_init(ec, 8);
		if (jent_rct_mem_init_ntg1(ec))
			return 1;
		break;
	case jent_health_init_type_common:
	default:
		if (jent_apt_init(ec))
			return 1;
		jent_rct_init(ec, 0);
		if (jent_rct_mem_init(ec))
			return 1;
		break;
	}

	return 0;
}
