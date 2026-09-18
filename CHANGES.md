3.7.1-prerelease
 * Jitter RNG core: the fallback to the internal timer is decided per collector and no longer forced process-wide once a startup needed it. The startup verdict is kept per clock, and a collector on a clock without one runs the startup on that clock first
 * Jitter RNG core: add jent_selftest to the API, running the SHA3-256 and XDRBG-256 known answer tests on their own. It is reentrant and can bind its verdict to a collector, which then permanently stops with the new JENT_ERR_SELFTEST
 * Jitter RNG core: add JENT_ERR_* definitions for all error codes returned by jent_read_entropy and jent_read_entropy_safe - the numeric values are unchanged
 * Jitter RNG core: drop the enhanced backtracking operation at the end of jent_read_entropy - the XDRBG-256 generate already consumes its state one-way
 * Jitter RNG core: fix the monotonicity check of jent_entropy_init*, which could not detect a timer running backwards
 * Jitter RNG core: fix reporting of a permanent RCT failure during jent_entropy_init* - it was reported as EHEALTH instead of ERCT
 * Jitter RNG core: establish the common timer divisor per clock rather than per process in jent_entropy_init*; a collector on a clock without a startup runs one first
 * Jitter RNG core: the memory access region is no longer secure memory, which exceeded common lock quotas on Android, Windows and in containers. It stays zeroed, guard-paged, excluded from core dumps and wiped
 * Jitter RNG core: the XDRBG state and output no longer pass through the stack, and entry points wipe the stack they used before returning
 * Jitter RNG core: without a CSPRNG the instance UUID is a version 8 UUID hashed with SHA3-256 from a process-wide counter and the current time
 * Jitter RNG core: with EXTERNAL_CRYPTO=AWSLC the collector state is secure memory where the platform can lock it - the AWS-LC allocation is locked (mlock / VirtualLock) and excluded from core dumps by the library
 * Jitter RNG core: support freestanding builds (-ffreestanding defines JENT_BAREMETAL) and report their memory as secure
 * Jitter RNG core: add UUID generation and update status printing
 * Jitter RNG core: try to pin the timer thread to one CPU
 * Jitter RNG core: add Linux kernel support header files and conditionally compile support code that is already offered by the Linux kernel
 * Jitter RNG core: jent_entropy_init_ex returns EPROGERR for an osr above JENT_MAX_OSR or memory access disabled in FIPS / NTG.1 mode, and a failing internal timer fallback no longer hides the platform timer's error behind EMEM
 * Jitter RNG core: a noise source that stops delivering ends the collection loop and is reported as JENT_ERR_RCT_PERMANENT in every mode. Outside FIPS and NTG.1 no health test ends that loop, so a clock that stopped after the startup spun forever - in the kernel module holding the instance lock
 * Jitter RNG core: jent_entropy_init* reports ENOMONOTONIC when no sampled reading reached the common timer divisor analysis, instead of marking the clock tested while leaving it without a divisor, which made every later collector on that clock fail with no way back
 * Health tests: implement the permanent failure of the lag predictor test (alpha=2^-44)
 * Health tests: the FIPS failure callback is invoked once per failure instead of on every check of it
 * Health tests: extend the APT cutoff tables from osr 15 to JENT_MAX_OSR
 * Health tests: fixes to the recovery loop of the RCT with memory and to the reallocation on health test failure, which now keeps the clock type of FIPS and NTG.1 instances
 * Health tests: add tests/health/health.c for induced failure testing of every health test and replaying recorded time stamps through them (issue #167)
 * Linux on Arm: derive the cache sizes from the core types in /proc/cpuinfo when neither sysfs nor sysconf reports them (e.g. on Android)
 * Linux on sysfs: an L4 cache (eDRAM) is no longer counted as L3, and the cache walk covers every CPU under musl
 * Cache detection: a reported cache size beyond a plausible bound is discarded rather than used. The x86 CPUID computation could overflow on a hypervisor answering the cache leaf with garbage, and a size at or above 4 GiB wrapped to zero in the combiner, which read as "no cache discoverable"
 * Linux kernel: add Linux kernel module support with hwrng, kcapi and character device interfaces
 * Linux kernel: max_kcapi_instances is a per-user limit, and the hwrng interface is left out against a kernel without CONFIG_HW_RANDOM
 * Linux kernel: a failing character device registration no longer unwinds the crypto API registration, which a concurrent AF_ALG bind could already hold a tfm against. The module loads without the device instead
 * Windows: the CPU count follows the thread affinity, and jent_fips_enabled() reports the system FIPS policy
 * Build: CMake 3.22 is the minimum version
 * Tests: EFI application for x86_64 and aarch64 running the library without an operating system
 * Tests: new fuzzing harness for the health tests, a thread sanitizer build, and tests for stack residue and exported symbols
 * Tests: Android and iOS example apps under tests/android and tests/ios, replacing arch/android
 * Tests: the health test cutoff tables are checked against their SP800-90B derivation (tests/health/cutoffs.py) as part of the suite, and the induced failure tests assert the cutoff boundary rather than reading the expected value out of the implementation
 * Build: the test programs depend on the library sources they include, so a changed source rebuilds them - make check could report success against a stale binary

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

