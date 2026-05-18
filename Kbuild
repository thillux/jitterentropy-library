# SPDX-License-Identifier: GPL-2.0-or-later
#
# Out-of-tree Linux kernel module build for jitterentropy_kmod.
#
# Build with:
#   make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
#
# or via the convenience wrapper:
#   make -f Makefile.kernel
#
# Optimisation policy: the kernel's macros (WARN_ON, copy_to_user, the
# asm_inline expansions in <asm/bug.h>, ...) rely on at least -O1 and
# break with "impossible constraint in 'asm'" under -O0. The jitter
# measurement on the other hand must run with -O0 - jitterentropy-base.c
# refuses to compile under __OPTIMIZE__.
#
# Resolution: build the kernel-API wrapper (jitterentropy_kmod.c) with
# the normal kernel CFLAGS and force only the library sources to -O0
# via CFLAGS_REMOVE_* + CFLAGS_*. We use CFLAGS_REMOVE_* because
# CFLAGS_<file>.o is appended to KBUILD_CFLAGS rather than replacing it
# - without the remove the existing -O2 would still win.

# $(M) is the absolute path to the external-module source tree (set by
# the kernel when invoked as `make M=...`). $(src) on modern kbuild is a
# relative path that does not survive being passed to the compiler from
# the kernel build directory, so resolve include directories through $(M)
# instead.
ccflags-y += -I$(M) -I$(M)/src -I$(M)/arch -I$(M)/kernel
ccflags-y += -DJENT_KERNEL -fwrapv

# Strip the kernel's default optimisation flags from the library sources
# (only) and substitute -O0. Some kernels also pass -Os/-O3 via configs
# such as CONFIG_CC_OPTIMIZE_FOR_SIZE; remove those too.
JENT_REMOVE_OPTFLAGS := -O1 -O2 -O3 -Os -Ofast -Og

CFLAGS_REMOVE_src/jitterentropy-base.o      = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-gcd.o       = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-health.o    = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-noise.o     = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-sha3.o      = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-status.o    = $(JENT_REMOVE_OPTFLAGS)
CFLAGS_REMOVE_src/jitterentropy-timer.o     = $(JENT_REMOVE_OPTFLAGS)

CFLAGS_src/jitterentropy-base.o      = -O0
CFLAGS_src/jitterentropy-gcd.o       = -O0
CFLAGS_src/jitterentropy-health.o    = -O0
CFLAGS_src/jitterentropy-noise.o     = -O0
CFLAGS_src/jitterentropy-sha3.o      = -O0
CFLAGS_src/jitterentropy-status.o    = -O0
CFLAGS_src/jitterentropy-timer.o     = -O0

obj-m := jitterentropy_kmod.o

jitterentropy_kmod-y := \
	kernel/jitterentropy_kmod.o \
	src/jitterentropy-base.o    \
	src/jitterentropy-gcd.o     \
	src/jitterentropy-health.o  \
	src/jitterentropy-noise.o   \
	src/jitterentropy-sha3.o    \
	src/jitterentropy-status.o  \
	src/jitterentropy-timer.o
