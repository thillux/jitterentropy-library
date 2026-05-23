/* SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause */
/*
 * libjent_status - tiny helper to read and print the JSON status of a
 * jitterentropy character device via the JENT_IOC_STATUS ioctl.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#ifndef LIBJENT_STATUS_H
#define LIBJENT_STATUS_H

#include <stdio.h>

#include "jitterentropy_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * jent_status_read_fd() - read the status from an already opened device fd
 * @fd:     file descriptor of a jitterentropy device
 * @status: caller-provided buffer filled with the JSON status on success
 *
 * Returns 0 on success or a negative errno value on failure.
 */
int jent_status_read_fd(int fd, struct jent_status *status);

/**
 * jent_status_read() - open a device and read its status
 * @device: device path; NULL defaults to "/dev/jitterentropy"
 * @status: caller-provided buffer filled with the JSON status on success
 *
 * Returns 0 on success or a negative errno value on failure.
 */
int jent_status_read(const char *device, struct jent_status *status);

/**
 * jent_status_print() - print the JSON status document to a stream
 * @stream: destination stream (e.g. stdout)
 * @status: status previously filled by jent_status_read()/_fd()
 */
void jent_status_print(FILE *stream, const struct jent_status *status);

#ifdef __cplusplus
}
#endif

#endif /* LIBJENT_STATUS_H */
