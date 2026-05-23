/* SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause */
/*
 * Userspace ABI for the jitterentropy character devices.
 *
 * This header is shared between the kernel module and userspace consumers
 * (e.g. the libjent_status helper library). It only uses the kernel UAPI
 * types and the ioctl helper macros so it is includable from both worlds.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#ifndef _JITTERENTROPY_UAPI_H
#define _JITTERENTROPY_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * Maximum size of the JSON status document returned by JENT_IOC_STATUS.
 * jent_status() never produces more than a few hundred bytes; the generous
 * buffer leaves head room for future fields while staying well below the
 * 14-bit ioctl size limit.
 */
#define JENT_STATUS_BUF_SIZE 4096

/**
 * struct jent_status - container for the JENT_IOC_STATUS ioctl
 * @length: number of bytes written to @json, excluding the trailing NUL
 * @json:   NUL-terminated JSON status document produced by jent_status()
 *
 * The caller passes a pointer to a zero-initialized instance of this struct
 * to ioctl(); on success the kernel fills in @json and @length.
 */
struct jent_status {
	__u32 length;
	char  json[JENT_STATUS_BUF_SIZE];
};

/* ioctl command space for the jitterentropy devices. */
#define JENT_IOC_MAGIC		'J'

/* Retrieve the JSON status of the backing Jitter RNG instance. */
#define JENT_IOC_STATUS		_IOR(JENT_IOC_MAGIC, 1, struct jent_status)

#endif /* _JITTERENTROPY_UAPI_H */
