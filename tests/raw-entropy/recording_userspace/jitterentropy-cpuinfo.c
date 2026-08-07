/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test tool listing the identification and cache layout of every CPU.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 *
 * On hybrid CPUs (Intel P/E cores, ARM big.LITTLE) the timing variations of
 * both noise sources depend on the micro-architecture and the caches of the
 * core the Jitter RNG runs on. A raw noise recording therefore only
 * characterizes the core type it was taken on.
 *
 * This tool prints the vendor, the model, the core type - where the system
 * reports one - and the data cache sizes of each CPU, so that the cores to be
 * measured individually can be identified. The measurement is then pinned to
 * one of them with "jitterentropy-hashtime --cpu <CPU>".
 *
 * How much can be reported depends on the operating system:
 *
 *   Linux    Complete. sysfs describes the caches and the topology of every
 *            CPU, and the identification is read on each core in turn.
 *   Windows  Complete. GetLogicalProcessorInformationEx() reports the caches,
 *            the topology and the efficiency class of every CPU, the registry
 *            holds their model.
 *   macOS    Complete, but per core type rather than per CPU: the cores are
 *            described per performance level (hw.perflevel<N>.*), which is how
 *            Apple Silicon exposes its P and E cores.
 *   Others   Partial. The BSDs and the remaining systems have no interface
 *            enumerating the caches of each CPU, so only the CPU this tool
 *            runs on is described, and its caches only on x86.
 *
 * Usage: jitterentropy-cpuinfo [--json]
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

/* Backend selection */
#if defined(_WIN32) || defined(_WIN64)
# define JENT_CPUINFO_WINDOWS
#elif defined(__linux__)
# define JENT_CPUINFO_LINUX
#elif defined(__APPLE__)
# define JENT_CPUINFO_MACOS
#elif defined(__unix__) || defined(__unix) || defined(__HAIKU__) || \
      defined(_AIX)
# define JENT_CPUINFO_GENERIC
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__) || \
    defined(_M_X64)     || defined(_M_IX86)
# define JENT_CPUINFO_X86
#elif defined(__aarch64__)
# define JENT_CPUINFO_ARM64
#endif

#ifdef JENT_CPUINFO_X86
# ifdef _MSC_VER
#  include <intrin.h>
# else
#  include <cpuid.h>
# endif
#endif

#define JENT_MAX_CPUS		1024
#define JENT_IDENT_LEN		160
#define JENT_TYPE_LEN		16

struct jent_cache_info {
	unsigned long size;	/* cache size in bytes, 0 if unknown */
	unsigned long shared;	/* number of CPUs sharing this cache */
};

struct jent_cpu_info {
	unsigned long cpu;
	int cpu_valid;		/* is the CPU number known? */
	long pkg;		/* physical package ID, -1 if unknown */
	long core;		/* core ID, -1 if unknown */
	unsigned long max_khz;	/* maximum CPU frequency, 0 if unknown */
	unsigned long base_khz;	/* nominal base frequency, 0 if unknown */
	unsigned long tsc_khz;	/* nominal timestamp counter rate, 0 if unknown */
	/* Timestamp counter properties, each 1 yes, 0 no, -1 not reported */
	int tsc_invariant;	/* constant rate across P-states (constant_tsc) */
	int tsc_nonstop;	/* keeps ticking in deep C-states (nonstop_tsc) */
	int tsc_known_freq;	/* rate is enumerated, not calibrated */
	struct jent_cache_info l1d, l1i, l2, l3;
	/* Vendor and model as one string, empty if the CPU is not identified */
	char ident[JENT_IDENT_LEN];
	/*
	 * Core type as reported by the system ("P-core"/"E-core") or the
	 * relative compute capacity known to the scheduler ("cap <N>").
	 */
	char type[JENT_TYPE_LEN];
};

struct jent_cpu_list {
	struct jent_cpu_info cpu[JENT_MAX_CPUS];
	long entries;		/* number of CPUs described below */
	long ncpu;		/* number of CPUs in the system */
	int pinning;		/* does the system offer CPU pinning? */
	int hypervisor;		/* 1 virtualized, 0 bare metal, -1 not known */
	/*
	 * Name of the backend the data comes from - what a consumer of the JSON
	 * output branches on, as it is what decides how complete the listing is.
	 */
	const char *backend;
	/* The same in prose, for the table output. NULL if there is nothing to say. */
	const char *note;
};

/* Set the fields whose "unknown" value is not zero. */
static void jent_cpu_info_init(struct jent_cpu_info *info)
{
	info->pkg = -1;
	info->core = -1;
	info->tsc_invariant = -1;
	info->tsc_nonstop = -1;
	info->tsc_known_freq = -1;
}

/* Defined by the backend for the operating system in use. */
static int jent_get_cpus(struct jent_cpu_list *list);

/***************************************************************************
 * x86 identification via CPUID
 *
 * - leaf 0 holds the vendor string in EBX, EDX, ECX,
 * - leaves 0x80000002 - 0x80000004 hold the processor brand string,
 * - leaf 0x1A (Intel SDM Vol. 2A, "Hybrid Information") reports the type of
 *   the core executing it in EAX[31:24]: 0x20 marks an Atom (efficiency) and
 *   0x40 a Core (performance) core. The leaf reads as zero on non-hybrid CPUs.
 *   These two are the only core types it knows - the low-power efficiency
 *   cores are Atom cores as well and are reported as such, so telling them
 *   apart is left to jent_mark_lp_cores().
 *
 * CPUID describes the core it is executed on, so on a hybrid CPU the caller
 * has to pin itself to the CPU it wants to identify first.
 ***************************************************************************/

#if defined(JENT_CPUINFO_X86) && !defined(JENT_CPUINFO_MACOS)

/*
 * Execute CPUID for @leaf/@subleaf into @regs (EAX, EBX, ECX, EDX). Returns
 * zero when the leaf is beyond what the CPU implements - the check the GCC and
 * clang helper does, spelled out for the MSVC intrinsics, which do not.
 */
static int jent_cpuid(unsigned int leaf, unsigned int subleaf,
		      unsigned int regs[4]);

/* Issue CPUID without asking whether the leaf is implemented. */
static void jent_cpuid_raw(unsigned int leaf, unsigned int subleaf,
			   unsigned int regs[4])
{
#ifdef _MSC_VER
	int out[4];

	__cpuidex(out, (int)leaf, (int)subleaf);
	regs[0] = (unsigned int)out[0];
	regs[1] = (unsigned int)out[1];
	regs[2] = (unsigned int)out[2];
	regs[3] = (unsigned int)out[3];
#else
	__cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
}

/*
 * The hypervisor leaves, 0x40000000 and up, are covered by neither the standard
 * nor the extended maximum, so they are guarded by their own two checks: a
 * hypervisor has to be present - CPUID.1 ECX[31], which no physical CPU sets -
 * and it has to implement the leaf, which its own 0x40000000 EAX states. On
 * bare metal an unimplemented leaf answers with the contents of the highest
 * standard one instead of zeroes, so asking without the checks yields
 * plausible-looking nonsense.
 */
static int jent_cpuid_hypervisor(unsigned int leaf, unsigned int subleaf,
				 unsigned int regs[4])
{
	unsigned int r[4];

	if (!jent_cpuid(1, 0, r) || !(r[2] & (1U << 31)))
		return 0;

	jent_cpuid_raw(0x40000000, 0, r);
	if (leaf > r[0])
		return 0;

	jent_cpuid_raw(leaf, subleaf, regs);

	return 1;
}

static int jent_cpuid(unsigned int leaf, unsigned int subleaf,
		      unsigned int regs[4])
{
	unsigned int max;

	if ((leaf & 0xFF000000U) == 0x40000000U)
		return jent_cpuid_hypervisor(leaf, subleaf, regs);

	/* The standard and the extended leaves have a maximum of their own. */
#ifdef _MSC_VER
	{
		int out[4];

		__cpuid(out, (int)(leaf & 0x80000000U));
		max = (unsigned int)out[0];
	}
#else
	/*
	 * Not __get_cpuid_count(): that one only appeared in GCC 4.9, while
	 * the distributions this is built on still carry 4.8. __get_cpuid_max()
	 * and the __cpuid_count() macro are what it is made of and have been
	 * there all along.
	 */
	max = __get_cpuid_max(leaf & 0x80000000U, NULL);
#endif

	if (leaf > max)
		return 0;

	jent_cpuid_raw(leaf, subleaf, regs);

	return 1;
}

/* Is a hypervisor present? CPUID.1 ECX[31], which no physical CPU sets. */
static int jent_hypervisor_present(void)
{
	unsigned int r[4];

	return (jent_cpuid(1, 0, r) && (r[2] & (1U << 31))) ? 1 : 0;
}

/* Windows names its CPUs from the registry and needs none of this. */
#if defined(JENT_CPUINFO_LINUX) || defined(JENT_CPUINFO_GENERIC)

static void jent_ident_x86(struct jent_cpu_info *info)
{
	unsigned int r[4];
	char vendor[13] = { 0 }, brand[49] = { 0 };
	const char *model = brand;

	if (!jent_cpuid(0, 0, r))
		return;
	memcpy(vendor,     &r[1], 4);
	memcpy(vendor + 4, &r[3], 4);
	memcpy(vendor + 8, &r[2], 4);

	{
		unsigned int regs[12], i;

		for (i = 0; i < 3; i++) {
			if (!jent_cpuid(0x80000002 + i, 0, &regs[i * 4]))
				break;
		}
		if (i == 3) {
			memcpy(brand, regs, sizeof(regs));
			/* The brand string is padded with leading blanks. */
			while (*model == ' ')
				model++;
		}
	}

	snprintf(info->ident, sizeof(info->ident), "%s%s%s", vendor,
		 *model ? " " : "", model);

	if (jent_cpuid(0x1A, 0, r) && r[0]) {
		switch (r[0] >> 24) {
		case 0x20:
			snprintf(info->type, sizeof(info->type), "E-core");
			break;
		case 0x40:
			snprintf(info->type, sizeof(info->type), "P-core");
			break;
		default:
			snprintf(info->type, sizeof(info->type), "0x%02x",
				 r[0] >> 24);
			break;
		}
	}
}

#endif /* JENT_CPUINFO_LINUX || JENT_CPUINFO_GENERIC */

/*
 * The nominal frequency some brand strings end in ("... CPU @ 2.40GHz"), in
 * kHz, or 0 when there is none. Only a trailing "@ <number><unit>Hz" is
 * accepted so that a model number containing a digit cannot be mistaken for a
 * frequency.
 */
static unsigned long jent_brand_khz(const char *brand)
{
	const char *at = strrchr(brand, '@');
	unsigned long value = 0, scale = 0, frac_digits = 0;
	int seen_digit = 0, in_frac = 0;

	if (!at)
		return 0;

	for (at++; *at == ' '; at++)
		;

	for (; *at; at++) {
		if (*at >= '0' && *at <= '9') {
			value = value * 10 + (unsigned long)(*at - '0');
			if (in_frac)
				frac_digits++;
			seen_digit = 1;
		} else if (*at == '.' && !in_frac) {
			in_frac = 1;
		} else {
			break;
		}
	}

	if (!seen_digit)
		return 0;

	if (*at == 'G')
		scale = 1000000;	/* GHz -> kHz */
	else if (*at == 'M')
		scale = 1000;		/* MHz -> kHz */
	else if (*at == 'k' || *at == 'K')
		scale = 1;
	else
		return 0;

	if (strncmp(at + 1, "Hz", 2))
		return 0;

	/* Undo the decimal point: "2.40GHz" parsed 240 with two digits. */
	for (; frac_digits; frac_digits--) {
		if (scale < 10)
			return 0;
		scale /= 10;
	}

	return value * scale;
}

/*
 * Frequencies and timestamp counter of the core this runs on.
 *
 * Leaf 0x16 (Intel SDM Vol. 2A, "Processor Frequency Information") reports
 * EAX[15:0] as the base and EBX[15:0] as the maximum frequency in MHz. Both
 * are nominal values the SDM does not promise to be exact, and on the hybrid
 * parts this was measured on only EBX follows the core the leaf is executed on
 * - EAX stays at one package-wide number while the P and the E cores have
 * markedly different base frequencies. The Linux backend therefore takes the
 * base frequency from cpufreq, which is per core, and reaches this leaf only
 * where that is unavailable.
 *
 * Leaf 0x15 ("Time Stamp Counter and Nominal Core Crystal Clock Information")
 * gives the TSC rate as ECX (the crystal clock in Hz) * EBX / EAX. The Jitter
 * RNG times its noise sources with that counter on x86, so its rate is the
 * resolution every measurement is bounded by.
 *
 * CPUID 0x80000007 EDX[8] marks an invariant TSC: one that ticks at a constant
 * rate across P-states and does not stop in deep C-states. Both vendors report
 * it, and it is what makes the counter usable as a time source at all.
 *
 * AMD implements neither 0x15 nor 0x16 - jent_cpuid() reports the leaves as
 * unsupported and the values stay unknown. There the TSC runs at the base
 * frequency of the part, which on Linux comes from cpufreq instead.
 */
static void jent_freq_x86(struct jent_cpu_info *info)
{
	unsigned int r[4];

	if (jent_cpuid(0x16, 0, r)) {
		if (!info->base_khz && (r[0] & 0xFFFF))
			info->base_khz = (unsigned long)(r[0] & 0xFFFF) * 1000;
		if (!info->max_khz && (r[1] & 0xFFFF))
			info->max_khz = (unsigned long)(r[1] & 0xFFFF) * 1000;
	}

	/*
	 * The brand string ends in the nominal frequency on the parts that
	 * state one ("... CPU @ 2.40GHz"), which is the base frequency. It is
	 * the last resort: on a part enumerating no 0x16 and a system with no
	 * cpufreq - a virtual machine, commonly - it is all there is.
	 */
	if (!info->base_khz)
		info->base_khz = jent_brand_khz(info->ident);

	if (jent_cpuid(0x15, 0, r) && r[0] && r[1] && r[2])
		info->tsc_khz = (unsigned long)((uint64_t)r[2] * r[1] /
						r[0] / 1000);

	/*
	 * Leaf 0x40000010, in kHz: the timing leaf VMware defined and several
	 * hypervisors implement after it. It is one of the few places a guest
	 * on an AMD host can learn the rate, as AMD enumerates neither of the
	 * two leaves above. Not every hypervisor has it - a KVM guest caps its
	 * leaf range below this one, and the check in jent_cpuid() then leaves
	 * the rate unknown rather than reading whatever the CPU answers with.
	 */
	if (!info->tsc_khz && jent_cpuid(0x40000010, 0, r) && r[0])
		info->tsc_khz = r[0];

	/*
	 * A rate that one of the two leaves enumerates is a known one; without
	 * them an operating system has to calibrate the counter against
	 * another timer, and what it arrives at is not what the CPU states.
	 */
	info->tsc_known_freq = info->tsc_khz ? 1 : 0;

	if (jent_cpuid(0x80000007, 0, r)) {
		/*
		 * The invariant-TSC bit covers both properties: such a counter
		 * ticks at a constant rate whatever the P-state and does not
		 * stop in the deep C-states. Linux splits them into its
		 * constant_tsc and nonstop_tsc flags, which it sets from this
		 * one bit as well - and additionally from model checks for the
		 * parts that predate it, which is why the Linux backend
		 * overrides these with what the kernel concluded.
		 */
		info->tsc_invariant = (r[3] & (1U << 8)) ? 1 : 0;
		info->tsc_nonstop = info->tsc_invariant;
	}
}

#endif /* JENT_CPUINFO_X86 && !JENT_CPUINFO_MACOS */

/*
 * The data cache geometry from CPUID, needed only where the operating system
 * has no interface of its own for it.
 *
 * Intel leaf 4 and the identically laid out AMD leaf 0x8000001D enumerate one
 * cache per sub-leaf (Intel SDM Vol. 2A, "Deterministic Cache Parameters";
 * AMD APM Vol. 3, "Cache Topology Information"):
 *
 *   EAX[ 4: 0]  cache type (0 = none/end, 1 = data, 2 = instruction, 3 = unified)
 *   EAX[ 7: 5]  cache level
 *   EBX[11: 0]  system coherency line size - 1
 *   EBX[21:12]  physical line partitions - 1
 *   EBX[31:22]  ways of associativity - 1
 *   ECX         number of sets - 1
 * Total size = (ways + 1) * (partitions + 1) * (line size + 1) * (sets + 1).
 *
 * The number of CPUs sharing a cache is deliberately not taken from EAX[25:14]:
 * that field is the number of addressable processor IDs, which is rounded up to
 * a power of two and describes what the encoding allows rather than what the
 * system has - on a 12-CPU part it reports 64 CPUs sharing the L3. The column
 * is left empty instead of filled with a number that is not the topology.
 */
#if defined(JENT_CPUINFO_X86) && defined(JENT_CPUINFO_GENERIC)

/* Walk the sub-leaves of @leaf; returns non-zero if any cache was found. */
static int jent_caches_x86_leaf(struct jent_cpu_info *info, unsigned int leaf)
{
	unsigned int sub;
	int found = 0;

	for (sub = 0; sub < 16; sub++) {
		unsigned int type, level, ways, partitions, line, sets;
		struct jent_cache_info *cache;
		unsigned int r[4], eax, ebx, ecx;

		if (!jent_cpuid(leaf, sub, r))
			break;
		eax = r[0];
		ebx = r[1];
		ecx = r[2];

		type = eax & 0x1F;
		if (type == 0)
			break;

		level      = (eax >> 5) & 0x7;
		ways       = ((ebx >> 22) & 0x3FF) + 1;
		partitions = ((ebx >> 12) & 0x3FF) + 1;
		line       = (ebx & 0xFFF) + 1;
		sets       = ecx + 1;

		if (level == 1 && type == 2)
			cache = &info->l1i;
		else if (level == 1 && (type == 1 || type == 3))
			cache = &info->l1d;
		else if (level == 2 && type != 2)
			cache = &info->l2;
		else if (level == 3 && type != 2)
			cache = &info->l3;
		else
			continue;

		cache->size = (unsigned long)ways * partitions * line * sets;
		found = 1;
	}

	return found;
}

static void jent_caches_x86(struct jent_cpu_info *info)
{
	/*
	 * Leaf 4 is Intel's. AMD and Hygon parts leave it empty - it reports
	 * cache type 0 in the first sub-leaf - and expose the identical
	 * structure through the extended leaf instead, gated by the
	 * TopologyExtensions feature; parts without it, and hypervisors hiding
	 * it, again report cache type 0 and leave the sizes at zero. A guest
	 * sees whichever leaf its host CPU implements, so both are probed
	 * rather than dispatching on the vendor ID. This mirrors what the
	 * library itself does in arch/jitterentropy-arch-cache.c.
	 */
	if (jent_caches_x86_leaf(info, 0x00000004))
		return;

	jent_caches_x86_leaf(info, 0x8000001D);
}

#endif /* JENT_CPUINFO_X86 && JENT_CPUINFO_GENERIC */

/***************************************************************************
 * Linux backend
 *
 * sysfs describes the caches and the topology of every CPU without any
 * privileges. Only the identification has to be read on the CPU itself, which
 * the affinity API allows.
 ***************************************************************************/

#ifdef JENT_CPUINFO_LINUX

#include <sched.h>

#define JENT_SYSFS_CPU		"/sys/devices/system/cpu"

/* Number of cache levels exposed per CPU in sysfs that are looked at. */
#define JENT_MAX_CACHE_INDEX	10

static int read_file_str(const char *path, char *buf, size_t buflen)
{
	FILE *f = fopen(path, "r");
	size_t len;

	if (!f)
		return -errno;

	if (!fgets(buf, (int)buflen, f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);

	/* Strip the trailing newline sysfs adds to every attribute. */
	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		buf[--len] = '\0';

	return len ? 0 : -ENODATA;
}

static int read_cpu_str(unsigned long cpu, const char *attr,
			char *buf, size_t buflen)
{
	char path[256];

	snprintf(path, sizeof(path), JENT_SYSFS_CPU "/cpu%lu/%s", cpu, attr);
	return read_file_str(path, buf, buflen);
}

/*
 * Read a numeric sysfs attribute. Base 0 is used so that both the decimal
 * attributes (core_id, cpuinfo_max_freq) and the hexadecimal ones
 * (midr_el1, given as 0x...) are handled.
 */
static int read_cpu_val(unsigned long cpu, const char *attr,
			unsigned long *val)
{
	char buf[64], *endptr;
	int ret = read_cpu_str(cpu, attr, buf, sizeof(buf));

	if (ret)
		return ret;

	errno = 0;
	*val = strtoul(buf, &endptr, 0);
	if (endptr == buf || errno != 0)
		return -EINVAL;

	return 0;
}

static int read_cpu_signed(unsigned long cpu, const char *attr, long *val)
{
	char buf[64], *endptr;
	int ret = read_cpu_str(cpu, attr, buf, sizeof(buf));

	if (ret)
		return ret;

	errno = 0;
	*val = strtol(buf, &endptr, 10);
	if (endptr == buf || errno != 0)
		return -EINVAL;

	return 0;
}

/*
 * Parse a kernel CPU list like "0-3,8" as used by /sys/.../online and the
 * shared_cpu_list cache attribute. When @cpus is given, the CPU numbers are
 * stored there, at most @max of them. The number of CPUs in the list is
 * returned, or a negative errno. A list longer than @max is reported as such
 * by returning the full count - the caller decides whether the truncation
 * matters.
 */
static long parse_cpu_list(const char *str, unsigned long *cpus, size_t max)
{
	const char *p = str;
	long count = 0;

	while (*p && *p != '\n') {
		char *endptr;
		unsigned long start, end, i;

		errno = 0;
		start = strtoul(p, &endptr, 10);
		if (endptr == p || errno != 0)
			return -EINVAL;
		p = endptr;

		if (*p == '-') {
			p++;
			errno = 0;
			end = strtoul(p, &endptr, 10);
			if (endptr == p || errno != 0 || end < start)
				return -EINVAL;
			p = endptr;
		} else {
			end = start;
		}

		for (i = start; i <= end; i++, count++) {
			if (cpus && (size_t)count < max)
				cpus[count] = i;
		}

		if (*p == ',')
			p++;
		else
			break;
	}

	return count ? count : -EINVAL;
}

/* Convert a sysfs cache size like "48K" or "32M" into bytes. */
static unsigned long parse_cache_size(const char *str)
{
	char *endptr;
	unsigned long val;

	errno = 0;
	val = strtoul(str, &endptr, 10);
	if (endptr == str || errno != 0)
		return 0;

	switch (*endptr) {
	case 'K':
		return val * 1024;
	case 'M':
		return val * 1024 * 1024;
	case 'G':
		return val * 1024 * 1024 * 1024;
	default:
		return val;
	}
}

static void jent_caches_linux(struct jent_cpu_info *info)
{
	unsigned int idx;

	for (idx = 0; idx < JENT_MAX_CACHE_INDEX; idx++) {
		char attr[64], type[32], size[32], list[512];
		struct jent_cache_info *cache;
		unsigned long level;
		long shared;

		snprintf(attr, sizeof(attr), "cache/index%u/level", idx);
		if (read_cpu_val(info->cpu, attr, &level))
			break;

		snprintf(attr, sizeof(attr), "cache/index%u/type", idx);
		if (read_cpu_str(info->cpu, attr, type, sizeof(type)))
			continue;

		/*
		 * The Jitter RNG only accesses data, so instruction caches are
		 * of no interest apart from documenting the L1 split. L2 and
		 * L3 are commonly unified.
		 */
		if (level == 1 && !strncmp(type, "Instruction", 11))
			cache = &info->l1i;
		else if (level == 1)
			cache = &info->l1d;
		else if (level == 2 && strncmp(type, "Instruction", 11))
			cache = &info->l2;
		else if (level == 3 && strncmp(type, "Instruction", 11))
			cache = &info->l3;
		else
			continue;

		snprintf(attr, sizeof(attr), "cache/index%u/size", idx);
		if (read_cpu_str(info->cpu, attr, size, sizeof(size)))
			continue;
		cache->size = parse_cache_size(size);

		/*
		 * The number of CPUs sharing a cache tells the P-core from the
		 * E-core clusters: on hybrid Intel CPUs, each P-core owns its
		 * L2 (shared with its SMT sibling at most) whereas a group of
		 * E-cores shares one.
		 */
		snprintf(attr, sizeof(attr), "cache/index%u/shared_cpu_list",
			 idx);
		if (read_cpu_str(info->cpu, attr, list, sizeof(list)))
			continue;
		shared = parse_cpu_list(list, NULL, 0);
		if (shared > 0)
			cache->shared = (unsigned long)shared;
	}
}

#ifdef JENT_CPUINFO_ARM64

/*
 * The rate of the architected generic timer, CNTFRQ_EL0, which is the
 * counterpart of the x86 timestamp counter: CNTVCT_EL0 is what the Jitter RNG
 * reads for its timings on this architecture, and CNTFRQ_EL0 states its
 * frequency. Both are readable at EL0.
 *
 * The counter is architecturally required to run at a constant frequency, to
 * be independent of the CPU clock and not to stop while the core is in a low
 * power state (Arm ARM (DDI 0487), "The system counter must be implemented in
 * an always-on power domain"), so the three properties below are answered from
 * the architecture rather than from a feature bit. The rate is commonly far
 * lower than an x86 TSC - tens of MHz - which is the resolution the noise
 * measurements are bounded by here.
 */
static void jent_timer_arm64(struct jent_cpu_info *info)
{
	uint64_t freq;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r" (freq));

	/* Firmware that leaves the register at zero has not set it up. */
	if (!freq)
		return;

	info->tsc_khz = (unsigned long)(freq / 1000);
	info->tsc_invariant = 1;
	info->tsc_nonstop = 1;
	info->tsc_known_freq = 1;
}

static const struct {
	unsigned long id;
	const char *name;
} arm_implementers[] = {
	{ 0x41, "ARM" },	{ 0x42, "Broadcom" },	{ 0x43, "Cavium" },
	{ 0x46, "Fujitsu" },	{ 0x48, "HiSilicon" },	{ 0x4e, "NVIDIA" },
	{ 0x50, "APM" },	{ 0x51, "Qualcomm" },	{ 0x53, "Samsung" },
	{ 0x56, "Marvell" },	{ 0x61, "Apple" },	{ 0x69, "Intel" },
	{ 0x6d, "Microsoft" },	{ 0x70, "Phytium" },	{ 0xc0, "Ampere" },
}, arm_parts[] = {
	/* Parts of the ARM implementer (0x41) */
	{ 0xd03, "Cortex-A53" },	{ 0xd04, "Cortex-A35" },
	{ 0xd05, "Cortex-A55" },	{ 0xd06, "Cortex-A65" },
	{ 0xd07, "Cortex-A57" },	{ 0xd08, "Cortex-A72" },
	{ 0xd09, "Cortex-A73" },	{ 0xd0a, "Cortex-A75" },
	{ 0xd0b, "Cortex-A76" },	{ 0xd0c, "Neoverse-N1" },
	{ 0xd0d, "Cortex-A77" },	{ 0xd40, "Neoverse-V1" },
	{ 0xd41, "Cortex-A78" },	{ 0xd44, "Cortex-X1" },
	{ 0xd46, "Cortex-A510" },	{ 0xd47, "Cortex-A710" },
	{ 0xd48, "Cortex-X2" },		{ 0xd49, "Neoverse-N2" },
	{ 0xd4a, "Neoverse-E1" },	{ 0xd4d, "Cortex-A715" },
	{ 0xd4e, "Cortex-X3" },		{ 0xd4f, "Neoverse-V2" },
	{ 0xd80, "Cortex-A520" },	{ 0xd81, "Cortex-A720" },
	{ 0xd82, "Cortex-X4" },		{ 0xd84, "Neoverse-V3" },
	{ 0xd85, "Cortex-X925" },	{ 0xd87, "Cortex-A725" },
	{ 0xd8e, "Neoverse-N3" },
};

static const char *arm_lookup(unsigned long id, int implementer)
{
	size_t i, entries = implementer ?
		sizeof(arm_implementers) / sizeof(arm_implementers[0]) :
		sizeof(arm_parts) / sizeof(arm_parts[0]);
	const char *name = NULL;

	for (i = 0; i < entries; i++) {
		if (implementer && arm_implementers[i].id == id) {
			name = arm_implementers[i].name;
			break;
		} else if (!implementer && arm_parts[i].id == id) {
			name = arm_parts[i].name;
			break;
		}
	}

	return name;
}

/*
 * AArch64 identification from MIDR_EL1, which the kernel exposes per CPU in
 * sysfs. The register holds the implementer in bits[31:24] and the part
 * number in bits[15:4] (Arm ARM (DDI 0487), MIDR_EL1). The part number is
 * what distinguishes the big from the LITTLE cores.
 */
static void jent_ident_linux(struct jent_cpu_info *info)
{
	unsigned long midr, impl, part, variant, revision, capacity;
	const char *impl_name, *part_name;
	char part_buf[16];

	/*
	 * First, and not after the MIDR: the register is read here, where the
	 * thread is pinned to the CPU being described, and a kernel that does
	 * not expose the MIDR in sysfs must not cost the timer rate as well.
	 */
	jent_timer_arm64(info);

	if (read_cpu_val(info->cpu, "regs/identification/midr_el1", &midr))
		return;

	impl     = (midr >> 24) & 0xff;
	part     = (midr >>  4) & 0xfff;
	variant  = (midr >> 20) & 0xf;
	revision =  midr        & 0xf;

	impl_name = arm_lookup(impl, 1);
	/* The part number space is implementer-specific. */
	part_name = (impl == 0x41) ? arm_lookup(part, 0) : NULL;
	if (!part_name) {
		snprintf(part_buf, sizeof(part_buf), "part 0x%03lx", part);
		part_name = part_buf;
	}

	if (impl_name)
		snprintf(info->ident, sizeof(info->ident), "%s %s r%lup%lu",
			 impl_name, part_name, variant, revision);
	else
		snprintf(info->ident, sizeof(info->ident),
			 "implementer 0x%02lx %s r%lup%lu", impl, part_name,
			 variant, revision);

	/*
	 * ARM reports no core type. What stands in for it is the capacity the
	 * scheduler works with, normalized so that the most capable core of the
	 * system is 1024 - derived from the capacity-dmips-mhz property of the
	 * device tree scaled by the maximum frequency, or from the CPPC
	 * performance values on the ACPI systems. It is what separates the big
	 * from the LITTLE cores; on a uniform system every core reports 1024
	 * and the value says only that they are equivalent.
	 */
	if (!read_cpu_val(info->cpu, "cpu_capacity", &capacity))
		snprintf(info->type, sizeof(info->type), "cap %lu", capacity);
}

#elif defined(JENT_CPUINFO_X86)

static void jent_ident_linux(struct jent_cpu_info *info)
{
	unsigned long perf;

	jent_ident_x86(info);
	jent_freq_x86(info);

	/*
	 * AMD reports no core type: its dense cores (Zen 4c/5c) are the same
	 * micro-architecture as the regular ones, with a smaller cache and a
	 * lower clock, and CPUID has no equivalent of Intel's hybrid leaf for
	 * them. Where the amd-pstate driver is in use, the highest performance
	 * level the firmware gives each core is the ranking that separates the
	 * preferred cores from the rest.
	 */
	if (!info->type[0] &&
	    !read_cpu_val(info->cpu, "cpufreq/amd_pstate_highest_perf", &perf))
		snprintf(info->type, sizeof(info->type), "perf %lu", perf);
}

#else /* neither x86 nor AArch64 */

/*
 * Generic identification from /proc/cpuinfo: the per-CPU block introduced by
 * "processor : <N>" is searched for the first key that names the CPU. Which
 * key that is differs per architecture (RISC-V uses "uarch", POWER "cpu",
 * s390 "machine"), and some architectures report nothing per CPU at all.
 */
static void jent_ident_linux(struct jent_cpu_info *info)
{
	static const char *keys[] = { "model name", "uarch", "cpu model",
				      "cpu", "machine" };
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[512];
	long cur = -1;

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *val = strchr(line, ':');
		size_t i, keylen;

		if (!val)
			continue;
		*val++ = '\0';
		while (*val == ' ' || *val == '\t')
			val++;
		val[strcspn(val, "\n")] = '\0';

		/* Strip the padding the file uses between key and colon. */
		keylen = strlen(line);
		while (keylen && (line[keylen - 1] == ' ' ||
				  line[keylen - 1] == '\t'))
			line[--keylen] = '\0';

		if (!strcmp(line, "processor")) {
			cur = strtol(val, NULL, 10);
			continue;
		}

		if (cur < 0 || (unsigned long)cur != info->cpu)
			continue;

		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			if (strcmp(line, keys[i]))
				continue;
			snprintf(info->ident, sizeof(info->ident), "%s", val);
			fclose(f);
			return;
		}
	}

	fclose(f);
}

#endif /* JENT_CPUINFO_ARM64 */

#ifdef JENT_CPUINFO_X86

/* Is @name one of the space separated words of @flags? */
static int has_flag(const char *flags, const char *name)
{
	size_t len = strlen(name);
	const char *p = flags;

	while ((p = strstr(p, name))) {
		if ((p == flags || p[-1] == ' ') &&
		    (p[len] == '\0' || p[len] == ' '))
			return 1;
		p += len;
	}

	return 0;
}

/*
 * The kernel's own view of the timestamp counter, taken from the flags line of
 * /proc/cpuinfo. It says more than CPUID does: constant_tsc and nonstop_tsc
 * come from the invariant-TSC bit but are also set from model checks on the
 * parts that predate it, and tsc_known_freq states that the rate was
 * enumerated rather than calibrated against another timer - the conclusion
 * Linux reached for the counter it drives its clocksource with, which is the
 * same counter the Jitter RNG reads.
 *
 * The file is walked once and the flags are applied to the CPU each block
 * belongs to: it holds one block per CPU, so re-reading it for every CPU would
 * be quadratic on a large machine.
 */
static void jent_tsc_flags_linux(struct jent_cpu_list *list)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[4096];
	long cur = -1, i;

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *val = strchr(line, ':');

		if (!val)
			continue;
		*val++ = '\0';
		while (*val == ' ' || *val == '\t')
			val++;
		val[strcspn(val, "\n")] = '\0';

		if (!strncmp(line, "processor", 9)) {
			cur = strtol(val, NULL, 10);
			continue;
		}

		if (cur < 0 || strncmp(line, "flags", 5))
			continue;

		for (i = 0; i < list->entries; i++) {
			struct jent_cpu_info *info = &list->cpu[i];

			if (!info->cpu_valid || info->cpu != (unsigned long)cur)
				continue;

			info->tsc_invariant = has_flag(val, "constant_tsc");
			info->tsc_nonstop = has_flag(val, "nonstop_tsc");
			info->tsc_known_freq = has_flag(val, "tsc_known_freq");
			break;
		}
	}

	fclose(f);
}

#endif /* JENT_CPUINFO_X86 */

/*
 * Whether this is a virtual machine, for the architectures whose CPU says
 * nothing about it - ARM has no equivalent of the x86 hypervisor bit that EL0
 * could read. Linux publishes what the firmware states about the machine in
 * the DMI attributes, and the device-tree systems name the hypervisor outright.
 *
 * Only a match is conclusive: a machine whose firmware happens to say nothing
 * is not thereby bare metal, so the answer stays unknown instead of turning
 * into a "no". The vendor strings matched are the ones that no physical
 * machine carries - "Microsoft Corporation" is not among them, as that is also
 * the vendor of the Surface hardware; for it the product name has to say
 * "Virtual Machine".
 */
static int jent_hypervisor_linux(void)
{
	static const char *vendors[] = {
		"QEMU", "Xen", "VMware", "innotek GmbH", "Parallels",
		"Amazon EC2", "Google", "Alibaba Cloud", "OpenStack",
		"Bochs", "Apple Virtualization", "Nutanix",
	};
	static const char *products[] = {
		"Virtual Machine", "VMware Virtual Platform", "VMware20,1",
		"KVM", "QEMU", "VirtualBox", "HVM domU", "Standard PC",
		"Google Compute Engine", "OpenStack", "Parallels",
	};
	char buf[128];
	size_t i;

	/* The device tree names it directly where there is one. */
	if (!read_file_str("/proc/device-tree/hypervisor/compatible", buf,
			   sizeof(buf)))
		return 1;

	if (!read_file_str("/sys/class/dmi/id/sys_vendor", buf, sizeof(buf))) {
		for (i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++) {
			if (!strncmp(buf, vendors[i], strlen(vendors[i])))
				return 1;
		}
	}

	if (!read_file_str("/sys/class/dmi/id/product_name", buf,
			   sizeof(buf))) {
		for (i = 0; i < sizeof(products) / sizeof(products[0]); i++) {
			if (!strncmp(buf, products[i], strlen(products[i])))
				return 1;
		}
	}

	return -1;
}

/*
 * Move the calling thread to @cpu. The identification of a hybrid CPU is only
 * meaningful when it is obtained on the core in question - CPUID leaf 0x1A
 * reports the type of the core executing it.
 */
static int pin_to_cpu(unsigned long cpu)
{
	cpu_set_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;

	CPU_ZERO(&set);
	CPU_SET((size_t)cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -errno;

	/*
	 * sched_setaffinity() migrates the calling thread before it returns,
	 * but a CPU that went offline in the meantime leaves the thread
	 * elsewhere - in which case the identification below would describe
	 * the wrong core.
	 */
	if (sched_getcpu() != (int)cpu)
		return -EAGAIN;

	return 0;
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	unsigned long cpu_ids[JENT_MAX_CPUS];
	char online[1024];
	long ncpu, i;
	int ret;

	ret = read_file_str(JENT_SYSFS_CPU "/online", online, sizeof(online));
	if (ret) {
		fprintf(stderr, "Cannot read " JENT_SYSFS_CPU "/online: %s\n",
			strerror(-ret));
		return ret;
	}

	ncpu = parse_cpu_list(online, cpu_ids, JENT_MAX_CPUS);
	if (ncpu < 0) {
		fprintf(stderr, "Cannot parse the list of online CPUs \"%s\"\n",
			online);
		return (int)ncpu;
	}

	list->ncpu = ncpu;
	list->pinning = 1;
	list->backend = "linux";
	if (ncpu > JENT_MAX_CPUS) {
		fprintf(stderr, "Only the first %d of %ld online CPUs are "
			"reported\n", JENT_MAX_CPUS, ncpu);
		ncpu = JENT_MAX_CPUS;
	}

	for (i = 0; i < ncpu; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		jent_cpu_info_init(info);
		info->cpu = cpu_ids[i];
		info->cpu_valid = 1;

		if (read_cpu_signed(info->cpu, "topology/physical_package_id",
				    &info->pkg))
			info->pkg = -1;
		if (read_cpu_signed(info->cpu, "topology/core_id", &info->core))
			info->core = -1;
		if (read_cpu_val(info->cpu, "cpufreq/cpuinfo_max_freq",
				 &info->max_khz))
			info->max_khz = 0;
		/*
		 * Exported by the intel-pstate and amd-pstate drivers, and the
		 * only source of the base frequency on AMD - CPUID has none.
		 */
		if (read_cpu_val(info->cpu, "cpufreq/base_frequency",
				 &info->base_khz))
			info->base_khz = 0;

		jent_caches_linux(info);

		/*
		 * The identification must run on the CPU it describes. When
		 * the tool is confined to a subset of the CPUs (taskset, a
		 * cpuset or a container), the remaining ones are reported with
		 * the data that sysfs provides for them.
		 */
		ret = pin_to_cpu(info->cpu);
		if (ret) {
			fprintf(stderr,
				"CPU %lu: cannot pin to it (%s) - no "
				"identification\n", info->cpu, strerror(-ret));
			continue;
		}

		jent_ident_linux(info);
	}

	list->entries = ncpu;

#ifdef JENT_CPUINFO_X86
	jent_tsc_flags_linux(list);
#endif

	/*
	 * Only where the CPU itself has no answer: on x86 the hypervisor bit is
	 * conclusive in both directions, and the firmware strings below are not.
	 */
	if (list->hypervisor < 0)
		list->hypervisor = jent_hypervisor_linux();

	return 0;
}

#endif /* JENT_CPUINFO_LINUX */

/***************************************************************************
 * macOS backend
 *
 * macOS offers no thread-to-CPU pinning and no per-CPU description, but it
 * groups the cores into performance levels: hw.perflevel0 is the fastest one,
 * which on Apple Silicon are the P cores, hw.perflevel1 the E cores. The
 * caches are reported per level, which is exactly the distinction this tool
 * exists for. Systems with a single core type (the Intel Macs) report no
 * levels at all and are described by the flat hw.* names instead.
 ***************************************************************************/

#ifdef JENT_CPUINFO_MACOS

#include <sys/types.h>
#include <sys/sysctl.h>

static int sysctl_str(const char *name, char *buf, size_t buflen)
{
	size_t len = buflen;

	if (sysctlbyname(name, buf, &len, NULL, 0))
		return -errno;
	buf[buflen - 1] = '\0';

	return 0;
}

/*
 * Read a numeric sysctl of either width - macOS reports some of these as
 * 32 bit and others as 64 bit values, so the width is taken from the node
 * itself rather than assumed.
 */
static int sysctl_num(const char *name, unsigned long long *val)
{
	size_t len = 0;

	if (sysctlbyname(name, NULL, &len, NULL, 0))
		return -errno;

	if (len == sizeof(uint32_t)) {
		uint32_t v = 0;

		len = sizeof(v);
		if (sysctlbyname(name, &v, &len, NULL, 0))
			return -errno;
		*val = v;
		return 0;
	}
	if (len == sizeof(uint64_t)) {
		uint64_t v = 0;

		len = sizeof(v);
		if (sysctlbyname(name, &v, &len, NULL, 0))
			return -errno;
		*val = v;
		return 0;
	}

	return -EINVAL;
}

/* Read hw.perflevel<level>.<attr>, or hw.<attr> when there are no levels. */
static int sysctl_level_num(int level, const char *attr,
			    unsigned long long *val)
{
	char name[64];

	if (level < 0)
		snprintf(name, sizeof(name), "hw.%s", attr);
	else
		snprintf(name, sizeof(name), "hw.perflevel%d.%s", level, attr);

	return sysctl_num(name, val);
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	unsigned long long nlevels = 0, freq = 0, ncpu = 0;
	/* Sized so that vendor, blank and model always fit into ident. */
	char ident[JENT_IDENT_LEN - 64] = "", vendor[63] = "";
	int level, levels;
	long n = 0;

	if (sysctl_num("hw.logicalcpu", &ncpu) || !ncpu)
		return -ENOENT;
	list->ncpu = (long)ncpu;

	/*
	 * The affinity tags macOS offers are a hint to keep threads together,
	 * not a way to place one on a given core, and they are not implemented
	 * at all on Apple Silicon.
	 */
	list->pinning = 0;
	list->backend = "macos";
	list->note =
		"macOS describes the cores per performance level, not per CPU, "
		"and offers no\nthread-to-CPU pinning: the CPU column holds "
		"the position in this listing, which\nstarts with the fastest "
		"level.";

	/* Present on Intel Macs only; Apple Silicon reports the model alone. */
	sysctl_str("machdep.cpu.vendor", vendor, sizeof(vendor));
	sysctl_str("machdep.cpu.brand_string", ident, sizeof(ident));
	if (sysctl_num("hw.cpufrequency_max", &freq))
		freq = 0;

	if (sysctl_num("hw.nperflevels", &nlevels) || nlevels < 2)
		nlevels = 0;
	levels = nlevels ? (int)nlevels : 1;

	for (level = 0; level < levels && n < JENT_MAX_CPUS; level++) {
		unsigned long long logical = 0, physical = 0, l1d = 0, l1i = 0;
		unsigned long long l2 = 0, l3 = 0, per_l2 = 0, i;
		/* Negative selects the flat hw.* names for a uniform system. */
		int sel = nlevels ? level : -1;
		char name[64], type[64] = "";

		if (sysctl_level_num(sel, "logicalcpu", &logical) || !logical)
			continue;
		if (sysctl_level_num(sel, "physicalcpu", &physical) ||
		    !physical)
			physical = logical;

		sysctl_level_num(sel, "l1dcachesize", &l1d);
		sysctl_level_num(sel, "l1icachesize", &l1i);
		sysctl_level_num(sel, "l2cachesize", &l2);
		if (sysctl_level_num(sel, "cpusperl2", &per_l2))
			per_l2 = 0;
		/* No L3 is reported per level; the flat node covers the Macs
		 * that have one. */
		if (sysctl_num("hw.l3cachesize", &l3))
			l3 = 0;

		if (nlevels) {
			snprintf(name, sizeof(name), "hw.perflevel%d.name",
				 level);
			if (sysctl_str(name, type, sizeof(type)))
				type[0] = '\0';
		}

		for (i = 0; i < logical && n < JENT_MAX_CPUS; i++, n++) {
			struct jent_cpu_info *info = &list->cpu[n];

			jent_cpu_info_init(info);
			info->cpu = (unsigned long)n;
			info->cpu_valid = 1;
			/* SMT siblings are consecutive on the Intel Macs. */
			info->core = (long)(i / (logical / physical));
			info->max_khz = (unsigned long)(freq / 1000);

			info->l1d.size = (unsigned long)l1d;
			info->l1d.shared = 1;
			info->l1i.size = (unsigned long)l1i;
			info->l1i.shared = 1;
			info->l2.size = (unsigned long)l2;
			info->l2.shared = (unsigned long)per_l2;
			info->l3.size = (unsigned long)l3;
			info->l3.shared = (unsigned long)ncpu;

			snprintf(info->ident, sizeof(info->ident), "%s%s%s",
				 vendor, (vendor[0] && ident[0]) ? " " : "",
				 ident);

			/*
			 * The level names are "Performance" and "Efficiency";
			 * fall back to the level order, which Apple documents
			 * as fastest first.
			 */
			if (type[0] == 'P' || (!type[0] && level == 0))
				snprintf(info->type, sizeof(info->type),
					 "P-core");
			else if (type[0] == 'E' || !type[0])
				snprintf(info->type, sizeof(info->type),
					 "E-core");
			else
				snprintf(info->type, sizeof(info->type), "%s",
					 type);
		}
	}

	if (!n)
		return -ENOENT;

	list->entries = n;

	return 0;
}

#endif /* JENT_CPUINFO_MACOS */

/***************************************************************************
 * Windows backend
 *
 * GetLogicalProcessorInformationEx() describes the caches, the cores and the
 * packages, each with the group affinity mask of the CPUs it covers, and
 * reports the efficiency class of every core - which is what Windows uses to
 * tell a P-core from an E-core. The model is not part of that interface and is
 * taken from the registry, where the kernel publishes it per CPU (and, unlike
 * CPUID, also on the ARM64 machines).
 ***************************************************************************/

#ifdef JENT_CPUINFO_WINDOWS

/*
 * GetLogicalProcessorInformationEx() is a Windows 7 API. Some toolchains still
 * default their target version to something older, which would hide it - the
 * same guard the library's own Win32 backends carry.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

/* Number of CPUs in a group affinity mask. */
static unsigned long affinity_count(KAFFINITY mask)
{
	unsigned long count = 0;

	while (mask) {
		count += mask & 1;
		mask >>= 1;
	}

	return count;
}

/*
 * Flat index of the CPU given by (group, bit). Windows numbers the CPUs per
 * processor group; the flat numbering used here counts the CPUs of the
 * preceding groups, which is the numbering jitterentropy-hashtime --cpu
 * expects as well.
 */
static long flat_cpu(const unsigned long *group_base, WORD groups,
		     WORD group, unsigned long bit)
{
	if (group >= groups)
		return -1;

	return (long)(group_base[group] + bit);
}

/* Apply @fn to every CPU covered by @mask. */
static void for_each_cpu(struct jent_cpu_list *list,
			 const unsigned long *group_base, WORD groups,
			 const GROUP_AFFINITY *mask,
			 void (*fn)(struct jent_cpu_info *, void *), void *ctx)
{
	unsigned long bit;

	for (bit = 0; bit < sizeof(KAFFINITY) * 8; bit++) {
		long cpu;

		if (!((mask->Mask >> bit) & 1))
			continue;

		cpu = flat_cpu(group_base, groups, mask->Group, bit);
		if (cpu < 0 || cpu >= list->entries)
			continue;

		fn(&list->cpu[cpu], ctx);
	}
}

struct cache_ctx {
	struct jent_cache_info cache;
	BYTE level;
	int instruction;
};

static void set_cache(struct jent_cpu_info *info, void *arg)
{
	const struct cache_ctx *ctx = (const struct cache_ctx *)arg;
	struct jent_cache_info *cache;

	if (ctx->level == 1 && ctx->instruction)
		cache = &info->l1i;
	else if (ctx->level == 1)
		cache = &info->l1d;
	else if (ctx->level == 2 && !ctx->instruction)
		cache = &info->l2;
	else if (ctx->level == 3 && !ctx->instruction)
		cache = &info->l3;
	else
		return;

	*cache = ctx->cache;
}

struct core_ctx {
	long index;
	BYTE efficiency_class;
};

static void set_core(struct jent_cpu_info *info, void *arg)
{
	const struct core_ctx *ctx = (const struct core_ctx *)arg;

	info->core = ctx->index;
	/* Held here until the highest class in the system is known. */
	info->max_khz = ctx->efficiency_class;
}

static void set_package(struct jent_cpu_info *info, void *arg)
{
	info->pkg = *(const long *)arg;
}

/* Model, vendor and maximum frequency as published by the kernel per CPU. */
static void jent_ident_windows(struct jent_cpu_info *info)
{
	/* Sized so that vendor, blank and model always fit into ident. */
	char key[128], name[JENT_IDENT_LEN - 64] = "", vendor[63] = "";
	DWORD len, mhz = 0;

	snprintf(key, sizeof(key),
		 "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%lu",
		 info->cpu);

	len = sizeof(vendor);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "VendorIdentifier",
			 RRF_RT_REG_SZ, NULL, vendor, &len) != ERROR_SUCCESS)
		vendor[0] = '\0';

	len = sizeof(name);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProcessorNameString",
			 RRF_RT_REG_SZ, NULL, name, &len) != ERROR_SUCCESS)
		name[0] = '\0';

	snprintf(info->ident, sizeof(info->ident), "%s%s%s", vendor,
		 (vendor[0] && name[0]) ? " " : "", name);

	len = sizeof(mhz);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "~MHz", RRF_RT_REG_DWORD,
			 NULL, &mhz, &len) == ERROR_SUCCESS)
		info->max_khz = mhz * 1000;
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf;
	unsigned long group_base[64];
	BYTE classes[JENT_MAX_CPUS];
	BYTE max_class = 0;
	WORD groups, group;
	DWORD len = 0;
	BYTE *ptr;
	long ncpu = 0, cores = 0, packages = 0, i;

	groups = GetActiveProcessorGroupCount();
	if (!groups)
		return -ENOENT;
	if (groups > (WORD)(sizeof(group_base) / sizeof(group_base[0])))
		groups = (WORD)(sizeof(group_base) / sizeof(group_base[0]));

	/*
	 * The CPUs of a group are numbered from zero, so the flat index of the
	 * first CPU of a group is the number of CPUs in all groups before it.
	 */
	for (group = 0; group < groups; group++) {
		group_base[group] = (unsigned long)ncpu;
		ncpu += (long)GetActiveProcessorCount(group);
	}

	list->ncpu = ncpu;
	list->pinning = 1;
	list->backend = "windows";
	if (ncpu > JENT_MAX_CPUS) {
		fprintf(stderr, "Only the first %d of %ld CPUs are reported\n",
			JENT_MAX_CPUS, ncpu);
		ncpu = JENT_MAX_CPUS;
	}
	list->entries = ncpu;

	for (i = 0; i < ncpu; i++) {
		jent_cpu_info_init(&list->cpu[i]);
		list->cpu[i].cpu = (unsigned long)i;
		list->cpu[i].cpu_valid = 1;
	}

	if (GetLogicalProcessorInformationEx(RelationAll, NULL, &len) ||
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -EFAULT;

	buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)malloc(len);
	if (!buf)
		return -ENOMEM;

	if (!GetLogicalProcessorInformationEx(RelationAll, buf, &len)) {
		free(buf);
		return -EFAULT;
	}

	for (ptr = (BYTE *)buf; ptr < (BYTE *)buf + len;) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p =
			(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)ptr;
		WORD n;

		/* A record of no length would spin here forever. */
		if (!p->Size)
			break;
		ptr += p->Size;

		/*
		 * Not a switch: the relationship is an enumeration the build
		 * compiles with -Wswitch-enum, which asks for every one of its
		 * values to be listed even though only three matter here.
		 *
		 * Trace caches hold no data and are skipped along with the
		 * relationships that are of no interest.
		 */
		if (p->Relationship == RelationCache &&
		    p->Cache.Type != CacheTrace) {
			struct cache_ctx ctx;

			ctx.level = p->Cache.Level;
			ctx.instruction = (p->Cache.Type == CacheInstruction);
			ctx.cache.size = (unsigned long)p->Cache.CacheSize;
			ctx.cache.shared =
				affinity_count(p->Cache.GroupMask.Mask);
			for_each_cpu(list, group_base, groups,
				     &p->Cache.GroupMask, set_cache, &ctx);
		} else if (p->Relationship == RelationProcessorCore) {
			struct core_ctx ctx;

			ctx.index = cores++;
			ctx.efficiency_class = p->Processor.EfficiencyClass;
			if (ctx.efficiency_class > max_class)
				max_class = ctx.efficiency_class;
			for (n = 0; n < p->Processor.GroupCount; n++)
				for_each_cpu(list, group_base, groups,
					     &p->Processor.GroupMask[n],
					     set_core, &ctx);
		} else if (p->Relationship == RelationProcessorPackage) {
			long pkg = packages++;

			for (n = 0; n < p->Processor.GroupCount; n++)
				for_each_cpu(list, group_base, groups,
					     &p->Processor.GroupMask[n],
					     set_package, &pkg);
		}
	}

	free(buf);

	/*
	 * The efficiency class is a relative ranking without a fixed scale, so
	 * it only names a core type once the highest class in the system is
	 * known: the performance cores hold the highest class and a uniform CPU
	 * reports one class for all. Everything below the top class is an
	 * efficiency core - there can be more than one such class, as on the
	 * parts that carry low-power E-cores next to the regular ones, which
	 * jent_mark_lp_cores() separates afterwards.
	 */
	for (i = 0; i < ncpu; i++) {
		classes[i] = (BYTE)list->cpu[i].max_khz;
		list->cpu[i].max_khz = 0;
	}

	for (i = 0; i < ncpu; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		if (max_class)
			snprintf(info->type, sizeof(info->type), "%s",
				 classes[i] == max_class ? "P-core" : "E-core");

		jent_ident_windows(info);
	}

#ifdef JENT_CPUINFO_X86
	/*
	 * The base frequency and the timestamp counter are only in CPUID, which
	 * answers for the core executing it - so unlike everything above, this
	 * has to visit each CPU. The affinity of this thread is restored
	 * afterwards from what the first call reports.
	 */
	{
		GROUP_AFFINITY previous;
		int restore = 0;

		for (group = 0; group < groups; group++) {
			unsigned long bit;

			for (bit = 0; bit < sizeof(KAFFINITY) * 8; bit++) {
				GROUP_AFFINITY affinity, old;
				PROCESSOR_NUMBER current;
				long cpu = flat_cpu(group_base, groups, group,
						    bit);

				if (cpu < 0 || cpu >= ncpu)
					continue;

				memset(&affinity, 0, sizeof(affinity));
				affinity.Group = group;
				affinity.Mask = (KAFFINITY)1 << bit;
				if (!SetThreadGroupAffinity(GetCurrentThread(),
							    &affinity, &old))
					continue;
				if (!restore) {
					previous = old;
					restore = 1;
				}

				/*
				 * A CPU parked or taken offline in the meantime
				 * leaves the thread where it was, and CPUID
				 * would then describe the wrong core.
				 */
				GetCurrentProcessorNumberEx(&current);
				if (current.Group != group ||
				    current.Number != (BYTE)bit)
					continue;

				jent_freq_x86(&list->cpu[cpu]);
			}
		}

		if (restore)
			SetThreadGroupAffinity(GetCurrentThread(), &previous,
					       NULL);
	}
#endif /* JENT_CPUINFO_X86 */

	return 0;
}

#endif /* JENT_CPUINFO_WINDOWS */

/***************************************************************************
 * Generic backend for the BSDs and everything else
 *
 * None of these systems enumerates the caches of the individual CPUs, and
 * OpenBSD deliberately offers no thread affinity API at all, so there is no
 * way to visit the cores in turn either. What is left is the CPU this tool
 * happens to run on: on x86 CPUID describes its caches and, on a hybrid CPU,
 * its core type, and on the BSDs hw.model names it.
 *
 * The numeric sysctl MIB is used rather than sysctlbyname(), which OpenBSD
 * does not provide. Solaris, Haiku and Cygwin have no sysctl at all, hence the
 * separate JENT_CPUINFO_HAVE_SYSCTL - they are left with the CPU count and,
 * where the architecture allows it, CPUID.
 ***************************************************************************/

#ifdef JENT_CPUINFO_GENERIC

#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
# include <sys/types.h>
# include <sys/param.h>
# include <sys/sysctl.h>
# define JENT_CPUINFO_HAVE_SYSCTL
/*
 * sysctlbyname() is on every BSD but OpenBSD, which offers the numeric MIB
 * alone. The MIB is used for what it covers - hw.model is CTL_HW/HW_MODEL
 * everywhere - and the name interface for the nodes that have no portable
 * MIB constant, such as the clock rate.
 */
# ifndef __OpenBSD__
#  define JENT_CPUINFO_HAVE_SYSCTLBYNAME
# endif
#endif

/*
 * FreeBSD is the one system reached here that can place a thread on a chosen
 * CPU, so it is the one whose CPUs can be described individually rather than
 * only the one this tool happens to run on. The call is the same one the
 * library pins its counting thread with (arch/jitterentropy-arch-thread.c).
 */
#ifdef __FreeBSD__
# include <sys/cpuset.h>
# define JENT_CPUINFO_BSD_AFFINITY

static int pin_to_cpu(unsigned long cpu)
{
	cpuset_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;

	CPU_ZERO(&set);
	CPU_SET((int)cpu, &set);
	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(set),
			       &set))
		return -errno;

	return 0;
}
#endif /* __FreeBSD__ */

#ifdef JENT_CPUINFO_HAVE_SYSCTL

static int sysctl_hw_str(int node, char *buf, size_t buflen)
{
	int mib[2] = { CTL_HW, node };
	size_t len = buflen;

	if (sysctl(mib, 2, buf, &len, NULL, 0))
		return -errno;
	buf[buflen - 1] = '\0';

	return 0;
}

#endif /* JENT_CPUINFO_HAVE_SYSCTL */

/* The clock rate in kHz, or 0 where the system does not report one. */
static unsigned long jent_clockrate(void)
{
#if defined(__OpenBSD__) && defined(HW_CPUSPEED)
	int mib[2] = { CTL_HW, HW_CPUSPEED };
	int speed = 0;
	size_t len = sizeof(speed);

	if (sysctl(mib, 2, &speed, &len, NULL, 0) || speed <= 0)
		return 0;

	return (unsigned long)speed * 1000;		/* MHz -> kHz */
#elif defined(JENT_CPUINFO_HAVE_SYSCTLBYNAME)
	/* hw.clockrate is FreeBSD and DragonFly, in MHz. */
	int speed = 0;
	size_t len = sizeof(speed);

	if (!sysctlbyname("hw.clockrate", &speed, &len, NULL, 0) && speed > 0)
		return (unsigned long)speed * 1000;

	/* NetBSD states the counter rate instead, in Hz. */
	{
		uint64_t freq = 0;

		len = sizeof(freq);
		if (!sysctlbyname("machdep.tsc_freq", &freq, &len, NULL, 0) &&
		    freq)
			return (unsigned long)(freq / 1000);
	}

	return 0;
#else
	return 0;
#endif
}

/* Everything that can be said about the CPU this thread is running on. */
static void jent_describe_current(struct jent_cpu_info *info)
{
#ifdef JENT_CPUINFO_HAVE_SYSCTL
	char model[JENT_IDENT_LEN] = "";

	if (!sysctl_hw_str(HW_MODEL, model, sizeof(model)) && model[0])
		snprintf(info->ident, sizeof(info->ident), "%s", model);
#endif

	info->base_khz = jent_clockrate();

#ifdef JENT_CPUINFO_X86
	/* Overrides hw.model with the vendor and brand string of this core. */
	jent_ident_x86(info);
	jent_caches_x86(info);
	jent_freq_x86(info);
#endif
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	long ncpu, n = 0;

	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	list->ncpu = (ncpu > 0) ? ncpu : 1;
	list->backend = "generic";

	/*
	 * OpenBSD is the one platform without any thread affinity API, so
	 * jitterentropy-hashtime --cpu is unavailable there.
	 */
#ifdef __OpenBSD__
	list->pinning = 0;
#else
	list->pinning = 1;
#endif

#ifdef JENT_CPUINFO_BSD_AFFINITY
	for (n = 0; n < list->ncpu && n < JENT_MAX_CPUS; n++) {
		struct jent_cpu_info *info = &list->cpu[n];

		/* Whatever was reached is kept; the note below says so. */
		if (pin_to_cpu((unsigned long)n))
			break;

		jent_cpu_info_init(info);
		info->cpu = (unsigned long)n;
		info->cpu_valid = 1;
		jent_describe_current(info);
	}
#endif

	if (!n) {
		struct jent_cpu_info *info = &list->cpu[0];

		jent_cpu_info_init(info);
		/* Without affinity, which CPU this is cannot be known. */
		info->cpu_valid = 0;
		jent_describe_current(info);
		n = 1;

		list->note =
			"This system has no interface describing the individual "
			"CPUs, so only the CPU\nthis tool runs on is reported - "
			"on a hybrid CPU, run the tool repeatedly to see\nthe "
			"other core types.";
	} else if (n < list->ncpu) {
		list->note =
			"Only the CPUs this tool could place itself on are "
			"described.";
	}

	list->entries = n;

	return 0;
}

#endif /* JENT_CPUINFO_GENERIC */

/***************************************************************************
 * Unsupported systems
 ***************************************************************************/

#if !defined(JENT_CPUINFO_LINUX) && !defined(JENT_CPUINFO_MACOS) && \
    !defined(JENT_CPUINFO_WINDOWS) && !defined(JENT_CPUINFO_GENERIC)

static int jent_get_cpus(struct jent_cpu_list *list)
{
	list->backend = "none";
	return -ENOSYS;
}

#endif

/*
 * Separate the low-power efficiency cores from the regular ones.
 *
 * Intel's LP E-cores (Meteor Lake and later) are Atom cores like the regular
 * efficiency cores and are reported as such: CPUID leaf 0x1A knows the two
 * core types Atom and Core and nothing else, and the efficiency class Windows
 * derives only ranks them. What sets them apart is where they sit - outside
 * the L3 domain, on the SoC tile or as the only E-cores of a part without an
 * L3 for them - so they are the efficiency cores that see no L3 on a system
 * whose other cores do.
 *
 * This is a derivation from the cache topology, not something the hardware
 * states, so it only ever refines an efficiency core that was identified as
 * one beforehand, and only when some other CPU does report an L3 - on a
 * machine that reports no L3 at all (a VM hiding it, a part without one)
 * nothing is renamed.
 */
static void jent_mark_lp_cores(struct jent_cpu_list *list)
{
	int have_l3 = 0;
	long i;

	for (i = 0; i < list->entries; i++) {
		if (list->cpu[i].l3.size) {
			have_l3 = 1;
			break;
		}
	}

	if (!have_l3)
		return;

	for (i = 0; i < list->entries; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		if (!strcmp(info->type, "E-core") && !info->l3.size)
			snprintf(info->type, sizeof(info->type), "LP-E-core");
	}
}

/***************************************************************************
 * Output
 ***************************************************************************/

/* Format a cache as "<size in KiB>/<CPUs sharing it>", e.g. "1280K/2". */
static void format_cache(const struct jent_cache_info *cache, char *buf,
			 size_t buflen)
{
	if (!cache->size) {
		snprintf(buf, buflen, "-");
		return;
	}

	if (cache->shared)
		snprintf(buf, buflen, "%luK/%lu", cache->size / 1024,
			 cache->shared);
	else
		snprintf(buf, buflen, "%luK", cache->size / 1024);
}

static void format_num(long val, char *buf, size_t buflen)
{
	if (val < 0)
		snprintf(buf, buflen, "-");
	else
		snprintf(buf, buflen, "%ld", val);
}

/* Summary of one tri-state CPU property over all CPUs. */
#define JENT_FLAG_NONE	(-1)	/* no CPU reports it */
#define JENT_FLAG_MIXED	(-2)	/* the CPUs disagree */

/* What the counter of this architecture is called. */
#ifdef JENT_CPUINFO_ARM64
# define JENT_TIMER_NAME	"Generic timer"
#else
# define JENT_TIMER_NAME	"Timestamp counter"
#endif

enum jent_tsc_prop {
	jent_tsc_invariant,
	jent_tsc_nonstop,
	jent_tsc_known_freq,
};

static int jent_tsc_flag(const struct jent_cpu_list *list,
			 enum jent_tsc_prop prop)
{
	int value = JENT_FLAG_NONE;
	long i;

	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];
		int cur;

		switch (prop) {
		case jent_tsc_nonstop:
			cur = info->tsc_nonstop;
			break;
		case jent_tsc_known_freq:
			cur = info->tsc_known_freq;
			break;
		case jent_tsc_invariant:
		default:
			cur = info->tsc_invariant;
			break;
		}

		if (cur < 0)
			continue;
		if (value == JENT_FLAG_NONE)
			value = cur;
		else if (value != cur)
			return JENT_FLAG_MIXED;
	}

	return value;
}

/* Does any CPU report @vendor? The model string starts with the vendor ID. */
static int jent_vendor_is(const struct jent_cpu_list *list, const char *vendor)
{
	size_t len = strlen(vendor);
	long i;

	for (i = 0; i < list->entries; i++) {
		if (!strncmp(list->cpu[i].ident, vendor, len))
			return 1;
	}

	return 0;
}

static const char *jent_flag_str(int value)
{
	switch (value) {
	case 1:
		return "yes";
	case 0:
		return "no";
	case JENT_FLAG_MIXED:
		return "differs between CPUs";
	default:
		return "unknown";
	}
}

static void print_cpus(const struct jent_cpu_list *list)
{
	const char *idents[JENT_MAX_CPUS];
	int nidents = 0, j;
	long i;

	printf("%4s %4s %5s %-9s %7s %7s %7s %10s %10s %10s %11s %6s\n",
	       "CPU", "Pkg", "Core", "Type", "BaseMHz", "MaxMHz", "TmrMHz",
	       "L1d", "L1i", "L2", "L3", "Model");

	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];
		char cpu[16], pkg[16], core[16], ident[16];
		char base[16], mhz[16], tsc[16];
		char l1d[24], l1i[24], l2[24], l3[24];

		if (info->cpu_valid)
			snprintf(cpu, sizeof(cpu), "%lu", info->cpu);
		else
			snprintf(cpu, sizeof(cpu), "?");
		format_num(info->pkg, pkg, sizeof(pkg));
		format_num(info->core, core, sizeof(core));
		format_num(info->max_khz ? (long)(info->max_khz / 1000) : -1,
			   mhz, sizeof(mhz));
		format_num(info->base_khz ? (long)(info->base_khz / 1000) : -1,
			   base, sizeof(base));
		format_num(info->tsc_khz ? (long)(info->tsc_khz / 1000) : -1,
			   tsc, sizeof(tsc));
		format_cache(&info->l1d, l1d, sizeof(l1d));
		format_cache(&info->l1i, l1i, sizeof(l1i));
		format_cache(&info->l2, l2, sizeof(l2));
		format_cache(&info->l3, l3, sizeof(l3));

		/*
		 * Identical models are listed once below the table. On hybrid
		 * x86 CPUs all cores report the same brand string and are told
		 * apart by the core type and the caches instead.
		 */
		if (!info->ident[0]) {
			snprintf(ident, sizeof(ident), "-");
		} else {
			for (j = 0; j < nidents; j++) {
				if (!strcmp(idents[j], info->ident))
					break;
			}
			if (j == nidents && nidents < JENT_MAX_CPUS)
				idents[nidents++] = info->ident;
			snprintf(ident, sizeof(ident), "#%d", j + 1);
		}

		printf("%4s %4s %5s %-9s %7s %7s %7s %10s %10s %10s %11s %6s\n",
		       cpu, pkg, core, info->type[0] ? info->type : "-", base,
		       mhz, tsc, l1d, l1i, l2, l3, ident);
	}

	printf("\nCache sizes are given in KiB, followed by the number of CPUs "
	       "sharing the cache.\n");
	printf("Type is the core type the system reports. Where it reports "
	       "none - on ARM - the\nrelative compute capacity the scheduler "
	       "was given is shown instead: 1024 is the\nmost capable core of "
	       "the system, and equal values mean equivalent cores.\n");
	printf("CPUs with the same Pkg and Core are SMT siblings of one "
	       "physical core and share\nits caches - measuring both of them "
	       "measures the same core.\n");
	printf("BaseMHz is the nominal base frequency, not what the CPU "
	       "currently runs at. TmrMHz\nis the rate of the counter the "
	       "Jitter RNG takes its timings from - the timestamp\ncounter on "
	       "x86, the architected generic timer on ARM.\n");

	/*
	 * The properties of the timestamp counter, which is what the Jitter RNG
	 * times its noise sources with on x86. They belong to the part rather
	 * than to a core, so they are summarized here instead of taking three
	 * more columns - and if the CPUs ever disagree, that is what is said.
	 */
	if (jent_tsc_flag(list, jent_tsc_invariant) != JENT_FLAG_NONE ||
	    jent_tsc_flag(list, jent_tsc_nonstop) != JENT_FLAG_NONE ||
	    jent_tsc_flag(list, jent_tsc_known_freq) != JENT_FLAG_NONE) {
		printf("%s: invariant/constant %s, nonstop %s, "
		       "known frequency %s\n", JENT_TIMER_NAME,
		       jent_flag_str(jent_tsc_flag(list, jent_tsc_invariant)),
		       jent_flag_str(jent_tsc_flag(list, jent_tsc_nonstop)),
		       jent_flag_str(jent_tsc_flag(list,
						   jent_tsc_known_freq)));
	}

	/*
	 * A dash in these columns has a reason, and next to a "known frequency
	 * yes" the reader deserves to be told which: the value exists, it is
	 * just not somewhere this tool can read it.
	 */
	{
		int have_freq = 0, have_tsc = 0;

		for (i = 0; i < list->entries; i++) {
			if (list->cpu[i].base_khz || list->cpu[i].max_khz)
				have_freq = 1;
			if (list->cpu[i].tsc_khz)
				have_tsc = 1;
		}

		if (!have_tsc &&
		    jent_tsc_flag(list, jent_tsc_known_freq) == 1) {
			printf("The counter rate is not enumerated by this "
			       "CPU, so only the operating system\nknows it");
			/*
			 * On AMD that is the normal case rather than a gap in
			 * this tool, which is worth saying outright.
			 */
			if (jent_vendor_is(list, "AuthenticAMD"))
				printf(" - AMD implements neither of the CPUID "
				       "leaves carrying it");
			printf(".\n");
		}

		if (!have_freq)
			printf("No frequency is reported: this CPU enumerates "
			       "none and the operating system\nhas no cpufreq "
			       "information for it either.\n");
	}

	if (list->hypervisor == 1)
		printf("Running under a hypervisor: the CPU described here is "
		       "the virtual one, and the\ntiming behavior of a "
		       "recording includes that of the host.\n");

	if (list->note)
		printf("\n%s\n", list->note);

	if (nidents) {
		printf("\nModels:\n");
		for (j = 0; j < nidents; j++)
			printf("  #%d: %s\n", j + 1, idents[j]);
	}

	if (list->pinning)
		printf("\nTo record the raw noise of one core, pin the "
		       "measurement to it:\n"
		       "  jitterentropy-hashtime <rounds> <repeats> <file> "
		       "--cpu <CPU>\n"
		       "The configuration a recording is made with is shown "
		       "with:\n"
		       "  jitterentropy-hashtime 1 1 unused --cpu <CPU> "
		       "--status\n");
	else
		printf("\nThis system offers no thread-to-CPU pinning, so "
		       "jitterentropy-hashtime cannot\nconfine a recording to "
		       "one core with --cpu.\n");
}

/*
 * JSON output
 *
 * Same data as the table above, for the scripts that drive a recording per
 * core type. Values the system does not report are null rather than absent, so
 * that every CPU has the same set of keys.
 */
static void print_json_string(const char *str)
{
	putchar('"');
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;

		switch (c) {
		case '"':
		case '\\':
			printf("\\%c", c);
			break;
		case '\n':
			printf("\\n");
			break;
		case '\t':
			printf("\\t");
			break;
		default:
			if (c < 0x20)
				printf("\\u%04x", c);
			else
				putchar(c);
			break;
		}
	}
	putchar('"');
}

/* A tri-state CPU property as a JSON literal. */
static const char *jent_json_flag(int value)
{
	if (value < 0)
		return "null";
	return value ? "true" : "false";
}

static void print_json_cache(const char *name,
			     const struct jent_cache_info *cache, int last)
{
	printf("\t\t\t\t\"%s\": ", name);

	if (!cache->size) {
		printf("null%s\n", last ? "" : ",");
		return;
	}

	printf("{ \"sizeBytes\": %lu, \"sharedCpus\": ", cache->size);
	if (cache->shared)
		printf("%lu", cache->shared);
	else
		printf("null");
	printf(" }%s\n", last ? "" : ",");
}

static void print_json(const struct jent_cpu_list *list)
{
	long i;

	printf("{\n");
	printf("\t\"cpus\": %ld,\n", list->ncpu);
	printf("\t\"pinning\": %s,\n", list->pinning ? "true" : "false");
	printf("\t\"backend\": ");
	print_json_string(list->backend ? list->backend : "none");
	printf(",\n");
	printf("\t\"hypervisor\": %s,\n", jent_json_flag(list->hypervisor));

	printf("\t\"processors\": [\n");
	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];

		printf("\t\t{\n");

		printf("\t\t\t\"cpu\": ");
		if (info->cpu_valid)
			printf("%lu,\n", info->cpu);
		else
			printf("null,\n");

		printf("\t\t\t\"package\": ");
		if (info->pkg < 0)
			printf("null,\n");
		else
			printf("%ld,\n", info->pkg);

		printf("\t\t\t\"core\": ");
		if (info->core < 0)
			printf("null,\n");
		else
			printf("%ld,\n", info->core);

		printf("\t\t\t\"coreType\": ");
		if (info->type[0])
			print_json_string(info->type);
		else
			printf("null");
		printf(",\n");

		printf("\t\t\t\"baseFrequencyKHz\": ");
		if (info->base_khz)
			printf("%lu,\n", info->base_khz);
		else
			printf("null,\n");

		printf("\t\t\t\"maxFrequencyKHz\": ");
		if (info->max_khz)
			printf("%lu,\n", info->max_khz);
		else
			printf("null,\n");

		printf("\t\t\t\"timer\": { \"frequencyKHz\": ");
		if (info->tsc_khz)
			printf("%lu", info->tsc_khz);
		else
			printf("null");
		printf(", \"invariant\": %s",
		       jent_json_flag(info->tsc_invariant));
		printf(", \"nonstop\": %s", jent_json_flag(info->tsc_nonstop));
		printf(", \"knownFrequency\": %s },\n",
		       jent_json_flag(info->tsc_known_freq));

		printf("\t\t\t\"model\": ");
		if (info->ident[0])
			print_json_string(info->ident);
		else
			printf("null");
		printf(",\n");

		printf("\t\t\t\"caches\": {\n");
		print_json_cache("l1d", &info->l1d, 0);
		print_json_cache("l1i", &info->l1i, 0);
		print_json_cache("l2", &info->l2, 0);
		print_json_cache("l3", &info->l3, 1);
		printf("\t\t\t}\n");

		printf("\t\t}%s\n", (i + 1 < list->entries) ? "," : "");
	}
	printf("\t]\n");
	printf("}\n");
}

static void usage(const char *name)
{
	printf("%s [--json]\n", name);
	printf("List the identification and the caches of all CPUs.\n\n");
	printf("  --json  report the same data as JSON\n");
	printf("  --help  print this text\n");
}

int main(int argc, char *argv[])
{
	static struct jent_cpu_list list;
	int json = 0, i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			json = 1;
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	list.hypervisor = -1;
#if defined(JENT_CPUINFO_X86) && !defined(JENT_CPUINFO_MACOS)
	list.hypervisor = jent_hypervisor_present();
#endif

	ret = jent_get_cpus(&list);
	if (ret) {
		fprintf(stderr, "Cannot obtain the CPU information: %s\n",
			strerror(-ret));
		return 1;
	}

	jent_mark_lp_cores(&list);

	if (json) {
		print_json(&list);
		return 0;
	}

	printf("CPUs: %ld\n\n", list.ncpu);
	print_cpus(&list);

	return 0;
}
