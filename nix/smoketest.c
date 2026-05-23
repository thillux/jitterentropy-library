// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * Minimal runtime smoke test for the userspace jitterentropy library.
 *
 * It runs the start-up self test, allocates an entropy collector and reads a
 * block of conditioned entropy. It is used by the Nix flake checks to verify
 * the library not only compiles but also runs against a given C library
 * (glibc / musl).
 *
 * Built together with the library sources with JENT_CONF_RELAX_MLOCK so the
 * test does not fail in build sandboxes that forbid mlock().
 */

#include <stdio.h>
#include <string.h>

#include "jitterentropy.h"

int main(void)
{
	struct rand_data *ec;
	unsigned char buf[64];
	ssize_t ret;

	if (jent_entropy_init() != 0) {
		fprintf(stderr, "jent_entropy_init() failed\n");
		return 1;
	}

	ec = jent_entropy_collector_alloc(1, 0);
	if (!ec) {
		fprintf(stderr, "jent_entropy_collector_alloc() failed\n");
		return 1;
	}

	memset(buf, 0, sizeof(buf));
	ret = jent_read_entropy_safe(&ec, (char *)buf, sizeof(buf));
	jent_entropy_collector_free(ec);

	if (ret != (ssize_t)sizeof(buf)) {
		fprintf(stderr, "jent_read_entropy_safe() returned %zd\n", ret);
		return 1;
	}

	printf("jitterentropy smoke test OK (version %u)\n", jent_version());
	return 0;
}
