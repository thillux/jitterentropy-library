// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * jitterentropy-status - print the JSON status of a jitterentropy device.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libjent_status.h"

int main(int argc, char **argv)
{
	const char *device = "/dev/jitterentropy";
	struct jent_status *status;
	int ret;

	if (argc > 1) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			fprintf(stderr,
				"Usage: %s [device]\n"
				"Print the JSON status of a jitterentropy device.\n"
				"  device  device node to query (default: %s)\n",
				argv[0], device);
			return 1;
		}
		device = argv[1];
	}

	/* struct jent_status is several KiB; keep it off the stack. */
	status = calloc(1, sizeof(*status));
	if (!status) {
		fprintf(stderr, "%s: out of memory\n", argv[0]);
		return 1;
	}

	ret = jent_status_read(device, status);
	if (ret) {
		fprintf(stderr, "%s: reading status from %s failed: %s\n",
			argv[0], device, strerror(-ret));
		free(status);
		return 1;
	}

	jent_status_print(stdout, status);
	free(status);
	return 0;
}
