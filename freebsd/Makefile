# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD kernel module build for jitterentropy_kmod.
#
# Build with:
#   make
#   make load          # kldload the freshly built module
#   make unload
#   make install
#
# This uses bsd.kmod.mk - the standard FreeBSD out-of-tree module build
# system. The jitterentropy core sources live in ../src and are pulled
# in via SRCS with .PATH so bsd.kmod.mk picks them up correctly.

.PATH: ${.CURDIR}/../src

KMOD    = jitterentropy_kmod
SRCS    = jitterentropy_kmod.c \
          jitterentropy-base.c \
          jitterentropy-gcd.c \
          jitterentropy-health.c \
          jitterentropy-noise.c \
          jitterentropy-sha3.c \
          jitterentropy-status.c \
          jitterentropy-timer.c

# The jitter measurement is timing-sensitive and jitterentropy-base.c
# refuses to compile under __OPTIMIZE__. Force -O0 + -fwrapv on every
# jitterentropy core object. Per-target CFLAGS overrides win against
# the KMOD-wide CFLAGS in bsd.kmod.mk.
CFLAGS += -I${.CURDIR}/.. -I${.CURDIR}/../src -I${.CURDIR}/../arch
CFLAGS += -DJENT_FREEBSD_KERNEL -fwrapv

CFLAGS.jitterentropy_kmod.c   = -O0
CFLAGS.jitterentropy-base.c   = -O0
CFLAGS.jitterentropy-gcd.c    = -O0
CFLAGS.jitterentropy-health.c = -O0
CFLAGS.jitterentropy-noise.c  = -O0
CFLAGS.jitterentropy-sha3.c   = -O0
CFLAGS.jitterentropy-status.c = -O0
CFLAGS.jitterentropy-timer.c  = -O0

.include <bsd.kmod.mk>
