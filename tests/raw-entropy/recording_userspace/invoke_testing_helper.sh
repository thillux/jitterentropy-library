#!/bin/sh

# Directory where to store the measurements
OUTDIR=${OUTDIR:-"../results-measurements"}

# Maximum number of entries to be extracted from the original file
NUM_EVENTS=${NUM_EVENTS:-1000000}

# Number of restart tests
NUM_EVENTS_RESTART=1000
NUM_RESTART=1000

NONIID_RESTART_DATA="jent-raw-noise-restart"
NONIID_DATA="jent-raw-noise"
NONIID_HASH_DATA="jent-raw-noise_hashloop"
NONIID_HASH_RESTART_DATA="jent-raw-noise-hashloop-restart"
NONIID_MEMLOOP_DATA="jent-raw-noise_memaccloop"
NONIID_MEMLOOP_RESTART_DATA="jent-raw-noise-memaccloop-restart"
IID_DATA="jent-conditioned.data"

JENT_HASHTIME=${JENT_HASHTIME:-"./jitterentropy-hashtime"}

# Define the maximum memory size
# 0 -> use default
# 1 -> JENT_MAX_MEMSIZE_1kB
# ...
# 20 -> JENT_MAX_MEMSIZE_512MB
MAX_MEMORY_SIZE=${MAX_MEMORY_SIZE:-0}

# If this variable is set to any value, the timer-less entropy source
# is forced and tested
FORCE_NOTIME_NOISE_SOURCE=""

# POSIX sh, as the device that records may have no bash, and ksh93 has no
# local: function variables are global, hence names no caller uses.

# A failed build or recording would leave a short or missing data set behind.
fail()
{
	echo "ERROR: $*" >&2
	exit 1
}

hashtime_build()
{
	make -s -f Makefile.hashtime || fail "building jitterentropy-hashtime failed"
}

# Arguments as for jitterentropy-hashtime, $3 is the output name.
hashtime_record()
{
	$JENT_HASHTIME "$@" || fail "jitterentropy-hashtime failed recording $3"
}

initialization()
{
	if [ ! -d $OUTDIR ]
	then
		mkdir $OUTDIR
		if [ $? -ne 0 ]
		then
			echo "Creation of $OUTDIR failed"
			exit 1
		fi
	fi

	# Keep the exit status of the script across the cleanup.
	trap 'rc=$?; make -s -f Makefile.rng clean; make -s -f Makefile.hashtime clean; exit $rc' 0
	trap 'exit 1' 1 2 3 15
}

lfsroutput()
{
	echo "Obtaining $NUM_EVENTS blocks of output from Jitter RNG"

	make -s -f Makefile.rng || fail "building jitterentropy-rng failed"

	cmdopts="--max-mem $MAX_MEMORY_SIZE"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	./jitterentropy-rng $NUM_EVENTS $cmdopts > $OUTDIR/$IID_DATA ||
		fail "jitterentropy-rng failed"

	make -s -f Makefile.rng clean
}

raw_entropy_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}


raw_entropy_ntg1_hash()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_HASH_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_hash_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_HASH_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_memacc()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_MEMLOOP_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_memacc_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_MEMLOOP_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}
