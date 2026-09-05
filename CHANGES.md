3.7.1-prerelease
 * Jitter RNG core: add jent_selftest to the API, running the SHA3-256 and XDRBG-256 known answer tests of the conditioning component on their own. jent_entropy_init* has always run them at startup; a long-running consumer such as the ESDM has to repeat them periodically, which so far meant re-running the whole startup including its statistical tests. The call is reentrant - stack-local state only, no allocation, no blocking - so it can run in parallel with entropy collection. The verdict can be bound to an entropy collector instance: on failure that instance permanently stops producing output, jent_read_entropy* returning the new JENT_ERR_SELFTEST error code in every mode of operation, not only under FIPS
 * Jitter RNG core: add JENT_ERR_* definitions for all error codes returned by jent_read_entropy and jent_read_entropy_safe - the numeric values are unchanged
 * Jitter RNG core: drop the enhanced backtracking operation at the end of jent_read_entropy, which ran on insecure memory only. Since the XDRBG-256 conversion in 3.7.0 every generated output block consumes the state one-way - the retained successor state cannot reproduce data already returned - so the extra empty generate defended nothing the construction does not already guarantee, and secure and insecure memory now behave identically
 * Jitter RNG core: fix the monotonicity check of jent_entropy_init*, which could not detect a timer running backwards. It compared the reading a measurement ended on against prev_time - delta, a reconstruction from an unsigned delta that jent_measure_jitter has already divided by the common timer divisor - so the comparison reduced to "delta > 0", which the coarseness check had already established. It now compares the readings the two measurements actually ended on, and ENOMONOTONIC is reachable
 * Jitter RNG core: fix reporting of a permanent RCT failure during jent_entropy_init* - it was reported as EHEALTH instead of ERCT
 * Jitter RNG core: publish the process-wide state - the self test verdict, the common timer GCD, the forced internal timer, the configuration switch blocks and the memoized cache geometry - through atomic load and store (new arch/jitterentropy-arch-atomic.{c,h}) instead of plain access. The registered FIPS failure callback goes through them as well: it is not a latch, and two threads registering at once - or one registering while another generates from a collector that would call it - raced on a function pointer the reader then called. Every one of them is a latch or a memo whose racing writers agree on the value, so the effect was benign, but they were data races by the memory model and a thread sanitizer reported each of them. jent_entropy_init and jent_entropy_init_ex may now be called from several threads at once; the configuration calls (notime CPU, notime implementation, FIPS failure callback) still have to precede any concurrent use
 * Jitter RNG core: establish the common timer divisor per clock rather than per process. The platform clock and the counting thread of the internal timer have unrelated granularities, so whichever startup ran first imposed its divisor on the other - truncating the jitter out of the counting thread's deltas, or leaving the platform clock's un-normalized
 * Jitter RNG core: refuse a collector whose clock no startup has measured, rather than substituting a divisor of one for it. Only the instances that do the measuring - the startup's own collector and the raw noise recording - run without one
 * Jitter RNG core: bound the entropy collection loop against a clock that stopped advancing. The loop repeats a stuck measurement until the health tests end it, and those report only under FIPS - so in every other mode a clock that froze after the startup measured it (a suspended VM, a hypervisor trapping the counter to a constant) made every measurement stuck and the loop never returned, in the kernel with the instance lock held. It now ends on a run of zero time deltas no working clock produces, and jent_read_entropy* reports it as JENT_ERR_RCT_PERMANENT in every mode rather than hanging
 * Jitter RNG core: where the platform offers no CSPRNG, an instance now has no UUID rather than the nil UUID. That was a well-formed identifier nothing downstream could tell from a generated one, and every instance shared it, which collided the per-instance /proc files of the kernel character device. The uuid status field is empty and JENT_IOCUUID gives ENODATA, as they already did for the raw test instances
 * Health tests: the recovery loop of the RCT with memory no longer switches the test off for the rest of the output block it is recovering. Each recovery block is a window of its own and reset the window counters of the outer block, leaving it past the end of its window, so no further measurement of that block was tested
 * Health tests: the recovery loop of the RCT with memory samples the noise source the current collection is measuring. It parked the startup state machine in the completed state, which sampled both noise sources - during the NTG.1 startup stages, which exist to sample the memory access and the SHA-3 loop separately, that injected the combined measurement into the pool mid-stage
 * Linux kernel: bound the concurrently open /dev/jitterentropy instances an unprivileged caller is allowed with the new max_instances module parameter (default 256, 0 for unlimited). The device is world readable and every open allocates a collector whose memory region is hundreds of kB - up to 512 MB with cache_all - so an unprivileged caller could exhaust kernel memory through opens alone. A caller with CAP_SYS_RESOURCE is not held to the cap, which keeps a full device administrable; the cap being global, one unprivileged caller can otherwise keep the next one out. The kernel allocations are now also charged to the caller's memory cgroup
 * Linux kernel: the new max_memsize module parameter bounds the memory access region of an instance (in kB, a power of two up to 512 MB). max_instances caps the number of instances, not the memory one of them ends up with: the reallocation on health-test recovery doubles the region, so a dozen recoveries take an instance to the 512 MB ceiling whatever it started at and with cache_all unset - measured at 17 recoveries from the default of a contemporary x86. Setting the parameter skips the cache-size derivation and keeps a recovery from growing the region, leaving it to raise the oversampling rate and the hash loop count
 * Linux kernel: /proc/jitterentropy/statistics, /proc/jitterentropy/hwrng_status and the instances/ directory with the files below it are readable by root only. They report how the device is being used - a specific instance's health state and how many bytes it has delivered, the number of instances open, and through the file names the UUIDs of those instances; the files describing the module configuration stay world readable
 * Linux kernel: the raw noise recording of the debugfs test interface yields the CPU and takes signals per measurement rather than per batch of 1000, and JENT_IOCLOOPCNT is bounded by the new JENT_LOOPCNT_MAX (1 << 18) rather than UINT_MAX. One measurement carries no reschedule point, so the loop count is what bounds the uninterruptible stretch of CPU a measurement occupies, and at UINT_MAX that is hours (measured in a VM: half a second per measurement at a loop count of 1 << 16, so ten hours at UINT_MAX) - the per-measurement yield alone would not have stopped it
 * Linux kernel: the raw noise recording re-primes the measurement after a reschedule. The delta of a measurement is computed from the time stamp taken during the previous one, so the scheduling gap would be recorded as if the noise source had produced it - the same reason the recording already re-primes after the gap spent copying a batch to userspace
 * Jitter RNG core: state the thread safety contract in jitterentropy.h and in the man page. The library takes no locks, and the only statement a consumer could find said the internal timer was "completely thread-safe as all relevant data is maintained as part of the entropy_collector data structure" - true of two different collectors, and easily read as licence to share one, which corrupts the conditioning state and the health test counters. The rules were written down only in comments inside the sources: one collector belongs to one thread at a time, the process-wide configuration calls must precede any concurrent use, jent_entropy_init* may run on several threads at once, and jent_selftest is reentrant
 * Jitter RNG core: the guard rejecting an out-of-range RCT-with-memory window now stops the output in every mode rather than only under FIPS. It returns having collected nothing, and the health failure bit it raised is silent outside a compliance mode, so jent_read_entropy() went on to emit a block from a pool that call never fed. Reachable only with JENT_MAX_OSR retuned far above its default
 * Tests: extractlsb creates its output file with mode 0644 instead of 0777. That file is the input an SP800-90B assessment is computed from, and the tool is installed by default
 * Tests: bound the range in the CPU list parser of jitterentropy-cpuinfo, as arch/jitterentropy-arch-ncpu.c already bounds its own. A list naming a range up to ULONG_MAX parsed without setting errno and was then walked, spinning while overflowing a signed count. No kernel presents such a list; the guard had simply not been copied along with the code
 * Jitter RNG core: the shared library no longer exports the internal jent_gcd_* functions on macOS and Windows. Their declarations carried JENT_PRIVATE_STATIC, which is the marker of the API in jitterentropy.h and expands to default visibility (or to dllexport), overriding the -fvisibility=hidden the library is built with. version.lds took them back out of the export set, but it is applied only where the linker takes a version script - not on Windows and not on macOS, where the shared library exported five internal functions taking raw pointers and sizes. Nothing wanted them: tests/gcd and the unit tests absorb these sources rather than link them
 * Jitter RNG core: the XDRBG successor state and the block returned to the caller no longer transit the stack. They were a local of the generate operation, so the one part of the state that JENT_FORCE_SECURE_MEM exists to keep out of swap and core dumps was the one part outside the locked, guard-paged allocation it asks for. The buffer is now a member of the conditioning context, which jent_sha3_alloc() obtains from that allocation; it is wiped after every generate as before
 * Tests: remove tests/raw-entropy/recording_runtime_kernelspace/attic. It records the execution time of jent_lfsr_time() and jent_fold_time(), which the replacement of the LFSR conditioning by SHA-3 removed before 3.0.0, so it has not built against the library for eight major releases - while shipping its own Makefiles and a README describing how to build it. It is in the history for anyone who needs it
 * Tests: the new exported-symbols test reads the built shared library and asserts that it exports the functions of version.lds and nothing else. The configure step already checks version.lds against jitterentropy.h, so this closes the loop from the header to the library that ships. It reads the build rather than the sources because that is where the two mechanisms meet: -fvisibility=hidden is overridden by JENT_PRIVATE_STATIC, and the version script that would have caught the result is applied for neither Windows nor macOS - which is how five internal functions came to be exported there. Registered for a shared build where nm is available, so it runs in the existing Linux and macOS CI jobs
 * Health tests: the reallocation on a health test failure no longer carries the APT base and the lag history when it changed the clock - those are delta values of a noise source the replacement is not reading. The RCT priming is a cutoff rather than anything the old clock produced and is carried as before
 * Health tests: a FIPS or NTG.1 instance keeps its clock across that reallocation. The recovery could otherwise fall back to the internal timer, continuing as a different noise source and taking every instance allocated afterwards with it
 * Jitter RNG core: select every arch/ backend for a freestanding build. -ffreestanding is what says so, both GCC and Clang reporting it by setting __STDC_HOSTED__ to zero, from which jitterentropy.h now defines JENT_BAREMETAL for every backend rather than only the thread one - without which an EFI application compiled on Linux would reach for mmap(), mlock(), sysconf(), sched_getaffinity() and getrandom() on a machine that has none of them. Such a build expects six functions from its integrator: memcpy(), memset(), malloc(), free(), strlen() and snprintf()
 * Jitter RNG core: report the memory of a baremetal build as secure. There is no swap device to page it out to, no second process to read it and no core dump for it to land in, which is the ground the Linux kernel backend already claims it on, and jent_zfree() wipes it on release as everywhere else. JENT_FORCE_SECURE_MEM is therefore satisfied rather than ignored there, and the compliance modes that imply it get memory that answers for it
 * Tests: two additions to the fuzzing. tests/fuzz/fuzz-health.c drives the SP800-90B health tests over time stamps the fuzzer chooses - nothing there measures the machine, so it runs about a thousand times faster than the API harness and reaches the counters, windows and cutoff tables the search could not get to before. tests/fuzz/fuzz-api.c now clamps the hash loop field of the flags it hands the allocation: it multiplies the conditioning of every time delta, and left unclamped the coverage-guided search collapsed onto inputs that time out, measuring under one execution per second
 * Tests: tests/unit/unit-concurrency.c releases its threads from a starting gate rather than where pthread_create() put them, covers both jent_read_entropy* entry points and jent_selftest(), and adds a second test racing the process-wide FIPS failure callback registration against the compliance-mode collectors that close it - which is what found the unsynchronized store to that callback. A new -DENABLE_THREAD_SANITIZER=ON build runs the suite under the thread sanitizer
 * Tests: unit-concurrency runs the two clocks against each other, unit-gcd checks that a divisor established for one clock is not handed to the other, and unit-error that a compliance-mode instance is not moved to the other clock
 * Tests: the library now builds and runs with no operating system under it. tests/efi is an EFI application for x86_64 and aarch64 - firmware, one processor, no scheduler, no libc, no CSPRNG - which puts the default configuration, JENT_FORCE_FIPS and JENT_NTG1 through the same sequence of allocation, 32 bytes of generation and jent_status(), the two compliance modes being allowed to be refused by their health test cutoffs, and which checks that JENT_FORCE_INTERNAL_TIMER is rejected where no thread can run a counter; `nix build .#checks.x86_64-linux.efi-vm` and its efi-vm-aarch64 companion boot both under EDK2 and check what they say. Note for anyone porting to aarch64 without an operating system: GCC 10 and later default to -moutline-atomics there, which turns the one read-modify-write in arch/jitterentropy-arch-atomic.c into a call to a libgcc helper that a -nostdlib link does not have, so such a build needs -mno-outline-atomics as the kernel does
 * Health tests: extend the APT cutoff tables from osr 15 to JENT_MAX_OSR, which every other cutoff table already covered. Above osr 15 the APT used to reuse the osr 15 entry; under NTG.1, where that entry is not yet the 512 cap, this made the test tighter than the oversampling rate calls for. A build assertion now fails the compilation of any table that does not cover the full osr range
 * Health tests: implement the permanent failure of the lag predictor test, which was defined as JENT_LAG_FAILURE_PERMANENT but never raised. The cutoffs use alpha=2^-44, the square of the intermittent alpha, following the convention of the RCT and APT
 * Health tests: add jent_health_insert_timestamp to run the health tests over externally obtained time stamps, so a raw entropy recording can be judged by the very tests that judge the noise source at runtime. Internal rather than part of the API
 * Health tests: the health tests can now be tested themselves with tests/health/health.c - the induced failure testing SP800-90B validations require, driving every health test to both its intermittent and its permanent cutoff, and replaying a file of time stamps through them with --replay (issue #167)
 * Jitter RNG core: apply LLM code review -> add sanity checks
 * Jitter RNG core: add UUID generation and update status printing
 * Jitter RNG core: try to pin the timer thread to one CPU
 * Jitter RNG core: add Linux kernel support header files and conditionally compile support code that is already offered by the Linux kernel

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

