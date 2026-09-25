3.7.1-prerelease
 * Jitter RNG core: the fallback to the internal timer is decided per collector and no longer forced process-wide once a startup needed it. The startup verdict is kept per clock, and a collector on a clock without one runs the startup on that clock first
 * Jitter RNG core: add jent_selftest to the API, running the SHA3-256 and XDRBG-256 known answer tests on their own. It is reentrant and can bind its verdict to a collector, which then permanently stops with the new JENT_ERR_SELFTEST and reports selftestFailed in jent_status
 * Jitter RNG core: add JENT_ERR_* definitions for all error codes returned by jent_read_entropy and jent_read_entropy_safe - the numeric values are unchanged
 * Jitter RNG core: drop the enhanced backtracking operation at the end of jent_read_entropy - the XDRBG-256 generate already consumes its state one-way
 * Jitter RNG core: fix the monotonicity check of jent_entropy_init*, which could not detect a timer running backwards
 * Jitter RNG core: fix reporting of a permanent RCT failure during jent_entropy_init* - it was reported as EHEALTH instead of ERCT
 * Jitter RNG core: establish the common timer divisor per clock rather than per process in jent_entropy_init*; a collector on a clock without a startup runs one first, and a startup with no sampled reading reaching the divisor analysis reports ENOMONOTONIC
 * Jitter RNG core: the memory access region is no longer secure memory, which exceeded common lock quotas on Android, Windows and in containers. It stays zeroed, guard-paged, excluded from core dumps and wiped
 * Jitter RNG core: the XDRBG state and output no longer pass through the stack, and entry points wipe the stack they used before returning
 * Jitter RNG core: with EXTERNAL_CRYPTO=AWSLC the collector state is secure memory where the platform can lock it - the AWS-LC allocation is locked (mlock / VirtualLock) and excluded from core dumps by the library
 * Jitter RNG core: support freestanding builds (-ffreestanding defines JENT_BAREMETAL) and report their memory as secure. Cores without lock-free 32-bit atomics (ARMv6-M, RISC-V without A) need no libatomic, MSVC builds compile, and an architecture without a known counter needs the internal timer compiled in
 * Jitter RNG core: add UUID generation and update status printing
 * Jitter RNG core: without a CSPRNG, or while a kernel RNG is not yet seeded, the instance UUID is a version 8 UUID hashed with SHA3-256 from a process-wide counter and the current time
 * Jitter RNG core: try to pin the timer thread to one CPU - the counting thread picks its default CPU from its own affinity
 * Jitter RNG core: add Linux kernel support header files and conditionally compile support code that is already offered by the Linux kernel
 * Jitter RNG core: jent_entropy_init_ex returns EPROGERR for an osr above JENT_MAX_OSR or memory access disabled in FIPS / NTG.1 mode, and a failing internal timer fallback no longer hides the platform timer's error behind EMEM
 * Jitter RNG core: a noise source that stops delivering ends the collection loop right away, also during the FIPS / NTG.1 startup and an RCT-with-memory recovery, and is reported as JENT_ERR_RCT_PERMANENT in every mode. Outside FIPS and NTG.1 no health test ends that loop, so a clock that stopped after the startup spun forever
 * Jitter RNG core: the startup's minimum-variation check counts in units of the timer divisor, so a clock's verdict no longer depends on whether a divisor was already stored, and only a passed startup stores the divisor
 * Jitter RNG core: the startup's own measuring collector no longer invokes the FIPS failure callback
 * Jitter RNG core: jent_entropy_set_notime_cpu and jent_entropy_switch_notime_impl return -EOPNOTSUPP instead of -1 without the internal timer
 * Jitter RNG core: jent_read_entropy_safe returns JENT_ERR_EINVAL for a NULL collector behind the handle also for a zero length, as jent_read_entropy does
 * Jitter RNG core: a failed jent_notime_init() of an external timer handler no longer has its leftover context passed to jent_notime_fini(), which could double free
 * Jitter RNG core: jent_read_entropy_safe carries the health test state, the UUID and the output counters into a replacement collector before its startup runs, so the startup continues the health tests instead of starting them afresh, and a FIPS failure callback fired during it sees the caller's instance
 * Jitter RNG core: a permanent health test failure during a collector's startup ends it instead of being retried at a higher osr, and is reported as permanent
 * Jitter RNG core: jent_read_entropy_safe returns a JENT_ERR_*_PERMANENT code when it cannot recover from an intermittent failure, and later calls on that collector report the same code; a replacement the platform cannot provide - memory, a counting thread - is not held against the noise source: the intermittent code is returned, the collector left as it was, and the next call tries again
 * Jitter RNG core: jent_entropy_init_ex and jent_entropy_collector_alloc refuse the reserved flag bits 9-22, a memory size field above JENT_MAX_MEMSIZE_MAX and a hash loop field above JENT_MAX_HASHLOOP (EPROGERR / NULL) instead of ignoring them
 * Jitter RNG core: a detected cache of 128 bytes or less counts as unknown, so the memory size is the default and grows from it on reallocation rather than shrinking to a few kB
 * Jitter RNG core: an RCT-with-memory recovery during the startup timing test samples the clock under test instead of the memory access stage
 * Jitter RNG core: on 32-bit targets a torn read of the internal timer's counter - a compiler storing the upper word first - is read again rather than taken, which yielded a huge delta or stalled the next read for some 2^32 increments
 * Jitter RNG core: the process-wide state - self test verdicts, timer divisors, the configuration switch blocks, the memoized cache geometry and the FIPS failure callback - is published through atomic loads and stores, and jent_entropy_init and jent_entropy_init_ex may be called from several threads at once; the configuration calls still have to precede any concurrent use
 * Jitter RNG core: the JENT_FORCE_SECURE_MEM flag replaces the compile-time option JENT_CONF_RELAX_MLOCK, which is no longer read: by default a collector is created even where its state cannot be locked, and with the flag the allocation fails instead
 * Jitter RNG core: with a compile-time JENT_HASH_LOOP_DEFAULT above 1, the health-test reallocation raises the hash loop count from that default instead of dropping it to 2, and keeps a default above 128
 * Jitter RNG core: the JENT_HASHLOOP_* flags are renumbered - API and ABI change: JENT_HASHLOOP_1 was field value 0, the same bits as no flag, and so selected the compile-time default rather than one loop. The field is now bits 23-26 (JENT_FLAGS_TO_HASHLOOP_SHIFT 23); value n selects 2^(n - 1) loops, 0 the default. Rebuild callers passing these flags: an old binary's field n reads as 2n, so its JENT_HASHLOOP_2 stays 2 loops, _4, _8 and _16 become 8, 32 and 128, and _32 to _128 are refused (EPROGERR); its JENT_HASHLOOP_1 remains the default
 * Health tests: implement the permanent failure of the lag predictor test (alpha=2^-44)
 * Health tests: the FIPS failure callback is invoked once per failure instead of on every check of it, also for an escalation to a permanent failure in jent_read_entropy_safe and for a noise source that stops during a replacement's startup
 * Health tests: extend the APT cutoff tables from osr 15 to JENT_MAX_OSR
 * Health tests: fixes to the recovery loop of the RCT with memory and to the reallocation on health test failure, which now keeps the clock type of FIPS and NTG.1 instances; jent_status still reports the flags the caller configured
 * Health tests: the RCT with memory of a reallocated collector continues from the replaced one's intermittent cutoff for its first window. The priming was stored but cleared by the window start before any cutoff saw it
 * Health tests: the global cutoffs of the lag predictor are one higher; firing at the quantile itself gave a false positive rate of about 2^-21.9 instead of 2^-22 (2^-44 permanent)
 * Health tests: a recovery block of the RCT with memory is judged when its window closes rather than failing at the intermittent cutoff, so a stuck source reaches the permanent cutoff where the NTG.1 tables leave it reachable (osr below 8) and is reported as JENT_ERR_RCT_MEM_PERMANENT. The recovery only starts inside the window
 * Health tests: the APT and the lag predictor restart at each FIPS / NTG.1 startup stage, which samples a different noise source; the stuck test keeps its reference deltas across that reset
 * Health tests: the ->prev_time priming measurement ahead of each block is kept out of the health tests. Its delta spans the time since the last measurement - after a startup the whole of its last measurement - and as the base symbol of an APT window it left that window unable to fire
 * Health tests: a FIPS / NTG.1 replacement collector no longer continues the APT window and lag history of the collector it replaces: its startup samples the memory access source alone, where the carried base left the APT blind for the rest of that window. It keeps the two deltas the stuck test compares with, so the RCT priming survives its first measurement
 * Health tests: add tests/health/health.c for induced failure testing of every health test at its cutoff boundary and replaying recorded time stamps through them (issue #167); the cutoff tables are checked against their SP800-90B derivation (tests/health/cutoffs.py), which JENT_REQUIRE_CUTOFF_CHECK makes a configure requirement
 * Linux on Arm: derive the cache sizes from the core types in /proc/cpuinfo when neither sysfs nor sysconf reports them (e.g. on Android)
 * Linux on sysfs: an L4 cache (eDRAM) is no longer counted as L3, and the cache walk covers every CPU under musl
 * Cache detection: cache sizes are uint64_t throughout. A size at or above 4 GiB wrapped to zero in the combiner, which read as "no cache discoverable"; the sum now saturates, and the memory size derived from it stays capped
 * Linux kernel: add Linux kernel module support with hwrng, kcapi and character device interfaces. Parameters max_instances (/dev/jitterentropy instances of unprivileged users, default 256, ENFILE beyond), max_kcapi_instances (per user and outermost container PID namespace) and max_memsize (kB, a power of two, at most 65536 on 32-bit kernels); an invalid osr, flags or memory size refuses the module load. The hwrng and crypto API interfaces are left out against a kernel without CONFIG_HW_RANDOM or CONFIG_CRYPTO_RNG2, and every interface returns EIO once a health test or self test failure is not recovered
 * Linux kernel: architectures without a counter instruction read random_get_entropy(), MIPS and m68k the raw clocksource via random_get_entropy_fallback() (5.19, 5.10.119, 5.15.44 and later), and RISC-V M-mode kernels random_get_entropy() instead of rdtime; arm64 cache levels below 4 KiB read from CCSIDR_EL1 count as unknown
 * Windows: the CPU count follows the thread affinity, and jent_fips_enabled() reports the system FIPS policy
 * POSIX: jent_ncpu() reports an error rather than 0 or a stale errno when sysconf() fails without setting errno
 * Apple platforms: aarch64 reads cntvct_el0 instead of clock_gettime_nsec_np(CLOCK_UPTIME_RAW), whose rescaling to nanoseconds hid the counter's quantum from the timer GCD, and the generic timer fallback uses clock_gettime(CLOCK_MONOTONIC) instead of mach_absolute_time()
 * FreeBSD kernel: arch backends of its own - malloc(9)/zfree(9) with M_NOWAIT, mp_ncpus, kern_yield(9), arc4random_buf(), rdtsc/get_cyclecount(), kernel CPUID - instead of the hosted POSIX paths, and no C library headers in jitterentropy.h, jitterentropy-status.c and jitterentropy-uuid.c
 * Build: CMake 3.22 is the minimum version
 * Build: a static CMake build no longer exports its -z now / -z relro linker flags to the consumers of jitterentropyTargets.cmake; the programs it builds and installs still link with them
 * Build: the CMake lists of compile and link flags start empty, so a parent project using add_subdirectory() cannot inject its own
 * Build: make install no longer strips the library, on macOS neither (INSTALL_STRIP="install -s" restores it, as strip -x there), which failed for cross builds with the host strip
 * Build: the test programs depend on the library sources they include, so a changed source rebuilds them - make check could report success against a stale binary
 * Build: the installed man page states the library version in its title and synopsis, and every function has an alias page, so that man jent_read_entropy finds it - with CMake and make install alike. INSTALL_MAN installs them only for targets that have man (off for Windows, Android, the Apple platforms other than macOS, bare metal and Emscripten)
 * Build: macOS CMake shared installs name the dylib by its absolute install path, and ELF tools get a $ORIGIN-relative rpath outside the default library directories, so installed tools run without DYLD_/LD_LIBRARY_PATH
 * Build: the installed jitterentropy.pc is relocatable (libdir and includedir relative to ${prefix}, the prefix set at install time), escapes blanks in its paths, names -ljitterentropy also when the library is add_subdirectory()'d, and make install installs it as well
 * Build: MSVC Release, RelWithDebInfo and MinSizeRel builds no longer inline helpers into the timed noise loop - /O1, /O2 and /Ox become /Od and /Ob1-/Ob3 become /Ob0 - and /GL, /LTCG and CMAKE_INTERPROCEDURAL_OPTIMIZATION are refused
 * Build: make install and make install-static build what they install instead of failing in a clean tree; the Makefile takes AARCH64_NSTIME_REGISTER and links with -z now on Solaris, as CMake does
 * Build: CI builds and tests on Linux, and builds the kernel module plain and through DKMS, on Ubuntu 22.04, 24.04 and 26.04, each on x86_64 and arm64
 * Build: README says NTG.1 needs no build options, what a build system other than CMake must pass on MSVC (/Od /Ob0, no /GL or /LTCG), and that timer ports go into arch/jitterentropy-arch-timer.c
 * Tests: unit tests under tests/unit for every module of src/ and arch/, including fault injection, concurrency under the thread sanitizer, stack residue, the wipe on release and the exported symbols
 * Tests: EFI application for x86_64 and aarch64 running the library without an operating system
 * Tests: fuzzing harnesses for the API and for the health tests, the latter framing blocks as the noise source does and covering the RCT-with-memory recovery
 * Tests: Android and iOS example apps under tests/android and tests/ios, replacing arch/android; the ndk-build library exports only the API
 * Tests: FreeBSD kernel module (tests/freebsd-kmod) that builds the library into a .ko and generates on kldload, run in a FreeBSD VM in CI
 * Tests: the restart validation in tests/raw-entropy caps each restart file at 1000 events rather than the whole data set, which left ea_restart 1000 of the 1000000 samples it needs, builds extractlsb itself, keeps a failed ea_restart sanity check verdict, reports min(H_r, H_c) rather than the constant H_I and no longer reports a previous run's results; ea_restart also writes its verdict as JSON
 * Tests: the raw entropy scripts run under any POSIX sh, fail when building, recording or an analysis fails, and stop on a signal; analyze_options.sh analyses each configuration and memory size into its own directory, and its plot leaves out a memory size without a result; a NixOS VM check runs them end to end
 * Tests: boottime_test_record.sh keeps the restart counter decimal, zero-padded only in file names, so run 8 no longer fails and the stop after $TESTS boots fires, and records time deltas with getrawentropy --timestamps from the kernel's jent_raw_hires interface instead of absolute time stamps
 * Tests: jitterentropy-osr reports JENT_MIN_OSR instead of failing an assertion when the linear estimate clamps to the minimum and measures above the target time
 * Tests: validation-restart/processdata.sh builds and removes extractlsb when run standalone, leaves it alone when a caller passes RESULTS_DIR, and honours NUM_EVENTS; the hashtime tool names every failure bit, and the rng and osr Makefiles build the internal timer

3.7.0
 * Add secure memory implementation for Linux and {Net,Open,Free}BSD, MacOS and Windows
 * Update supported CMake version to 3.10
 * doc: use Doxygen-style comments
 * NTG.1 compliance: Modify startup such that the memory access and SHA-3 loop are treated as independent noise sources which are sampled to collect at least 240 bits each before first block of random numbers is released
 * Remove all code when JENT_CONF_DISABLE_LOOP_SHUFFLE is unset. This code is already discouraged for a long time. Now it is taken out for good.
 * If cache size cannot be detected from base system (e.g. virtualization), use the requested memory size.
 * Change the stuck test to always calculate the absolute values of the 2nd and 3rd discrete derivation of time.
 * Replace SHA3-256 output generation with XDRBG-256
 * Prune the jitterentropy.h header file of internal definitions and delcarations which are moved to src/jitterentropy-internal.h. With that, jitterentropy.h only contains the API. This modification does not alter the Jitter RNG behavior at all.
 * Update secure storage memory implementation for libgcrypt and OpenSSL
 * Add API jent_status

3.6.3
 * Correct time stamp processing on AIX
 * Use high-resolution time stamp on Apple Silicon
 * GCD power-up test: consider OSR

3.6.2
 * Fix RCT re-initialization in jent_read_entropy_safe (thanks to Joshua Hill for pointing this out)
 * simplify test code
 * improve keyword portability

3.6.1
 * Add more test code
 * Add support for SunPRO compiler
 * Fix compilation on OpenBSD by replacing sed with tr
 * internal timer: Add support for Apple
 * Various small fixes to compilation to imporve portability

3.6.0
 * Remove bi-modal behavior of conditioning function
 * Make jent_read_entropy_safe safer by retrying the health test
 * Move the version information to make them available at compile time

3.5.0
 * add distinction between intermittent and permanent health failure

 * add compile time option to allow configuring a mask to reduce the size of
   the time stamp used for the APT

3.4.1
 * add FIPS 140 hints to man page
 * simplify the test tool to search for optimal configurations
 * fix: jent_loop_shuffle: re-add setting the time that was lost with 3.4.0
 * enhancement: add ARM64 assembler code to read high-res timer

3.4.0
 * enhancement: add API call jent_set_fips_failure_callback as requested by Daniel Ojalvo
 * fix: Change the SHA-3 integration: The entropy pool is now a SHA-3 state.
It is filled with the time delta containing entropy and auxiliary data that does not contain entropy using a SHA update operation. The auxiliary data is calculated by a SHA-3 hashing of some varying state data. The time delta that contains entropy is measured about the SHA-3 hasing of the auxiliary data. This satisfies FIPS 140-3 IG D.K resolutions 4, 6, and 8.
 * enhancement: add CMake support by Andrew Hopkins

3.3.1
 * fix: bug fix in initialization logic by Vladis Dronov <vdronov@redhat.com>
 * fix: use __asm__ instead of asm to suit the C11 standard

3.3.0
 * add jent_get_cachesize if _SC_LEVEL1_DCACHE_SIZE is not defined
 * limit the memory buffer size allocated and allow caller to provide
   the means to provide a limit, too
 * fix: update man page
 * update README explaining how to handle entropy shortfall to make it consistent with the current code base

3.2.0
 * fix: add API call jent_read_entropy_safe to header file
 * enhancement: add jent_entropy_init_ex API call
 * enhancement: call jent_entropy_init_ex automatically when jent_entropy_collector_alloc_internal detects that no self test has yet been performed
 * test: provide jitterentropy-rng test tool allowing all options exported by the library to be invoked
 * fix: re-add check of time_backwards in power-on test
 * fix: silence static code analysis tool
 * test: add test for GCD
 * enhancement: add GCD selftest
 * fix: simplify memory management for SHA-3
 * enhancement: add random memory access (JENT_RANDOM_MEMACCESS)

3.1.0
 * Add link call to pthreads library as suggested by Mikhail Novosyolov
 * Add ENTROPY_SAFETY_FACTOR to apply consideration of asymptotically reaching
   full entropy following SP800-90C suggested by Joshua Hill
 * Add test for finiding more entropy by changing the memory buffer size
   used for the memory access loop
 * Increase the memory buffer size to 512 kBytes per default based on
   measurements on systems with low entropy.
 * Add jent_ncpu() detecting the number of existing CPUs. Only when more than
   one CPU is in the system, the internal timer thread is started.
 * add GCD testing and analysis suggested by Joshua Hill
 * add fixes to APT suggested by Joshua Hill
 * add lag predictor health test suggested by Joshua Hill
 * add jent_read_entropy_safe API call
 * break up jitterentropy-base.c into various smaller code files

3.0.2
 * Small fixes suggested by Joshua Hill
 * Update the invocation of SHA-3 invocation: each loop iteration defined by the loop shuffle is a self-contained SHA-3 operation. Therefore, the conditioning information is always *one* SHA-3 operation with different time duration.
 * add JENT_CONF_DISABLE_LOOP_SHUFFLE config option allowing disabling of the shuffle operation
 * Use -O0

3.0.1
 * on older GCC versions use -fstack-protector as suggested by Warszawski,
   Diego
 * prevent creating the internal timer thread if a high-res hardware timer is
   found as reported by Lonnie Abelbeck

3.0.0
 * use RDTSC on x86 directly instead of clock_gettime
 * use SHA-3 instead of LFSR
 * add internal high-resolution timer support

2.2.0
 * SP800-90B compliance: Add RCT runtime health test
 * SP800-90B compliance: Add Chi-Squared runtime health test as a replacement
   for the adaptive proportion test
 * SP800-90B compliance: Increase initial entropy test to 1024 rounds
 * SP800-90B compliance: Invoke runtime health tests during initialization
 * remove FIPS 140-2 continuous self test (RCT covers the requirement as per
   FIPS 140-2 IG 9.8)
 * SP800-90B compliance: Do not mix stuck time deltas into entropy pool

2.1.2:
 * Add static library compilation thanks to Neil Horman
 * Initialize variable ec to satisfy valgrind as suggested by Steve Grubb
 * Add cross-compilation support suggested by Lonnie Abelbeck

2.1.1:
 * Fix implementation of mathematical properties.

2.1.0:
 * Convert all __[u|s][32|64] into [uint|int][32|64]_t
 * Remove all code protected by #if defined(__KERNEL__) && !defined(MODULE)
 * Add JENT_PRIVATE_COMPILE: Enable flag during compile when
   compiling a private copy of the Jitter RNG
 * Remove unused statistical test code
 * Add FIPS 140-2 continuous self test code
 * threshold for init-time stuck test configurable with JENT_STUCK_INIT_THRES
   during compile time

2.0.1:
 * Invcation of stuck test during initalization

2.0.0:
 * Replace the XOR folding of a time delta with an LFSR -- the use of an
   LFSR is mathematically more sound for the argument to maintain entropy

1.2.0:
 * Use constant time operation of jent_stir_pool to prevent leaking
   timing information about RNG.
 * Make it compile on 32 bit archtectures

1.1.0:
 * start new numbering schema
 * update processing of bit that is deemed holding no entropy by heuristic:
   XOR it into pool without LSFR and bit rotation (reported and suggested
   by Kevin Fowler <kevpfowler@gmail.com>)

