// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * libjent_status - see libjent_status.h.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

/* for O_CLOEXEC under -std=c11 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "libjent_status.h"

int jent_status_read_fd(int fd, struct jent_status *status)
{
	if (!status)
		return -EINVAL;

	memset(status, 0, sizeof(*status));
	if (ioctl(fd, JENT_IOC_STATUS, status) < 0)
		return -errno;

	/* Defensively guarantee NUL-termination before the buffer is used. */
	status->json[sizeof(status->json) - 1] = '\0';
	return 0;
}

int jent_status_read(const char *device, struct jent_status *status)
{
	int fd, ret;

	if (!device)
		device = "/dev/jitterentropy";

	fd = open(device, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	ret = jent_status_read_fd(fd, status);
	close(fd);
	return ret;
}

void jent_status_print(FILE *stream, const struct jent_status *status)
{
	size_t len;

	if (!stream || !status)
		return;

	fputs(status->json, stream);

	/* Append a trailing newline only if the document does not end in one. */
	len = strlen(status->json);
	if (len == 0 || status->json[len - 1] != '\n')
		fputc('\n', stream);
}
