// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * Build wrapper: compile the unmodified jitterentropy library source
 * src/jitterentropy-noise.c as part of the kernel module. The wrapper keeps
 * the upstream sources untouched and lets Kbuild build them in this
 * directory (with -O0, as the Jitter RNG requires).
 */
#include "../src/jitterentropy-noise.c"
