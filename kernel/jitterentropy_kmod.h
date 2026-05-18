/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UAPI for the out-of-tree jitterentropy_kmod kernel module.
 *
 * Users of /dev/jitterentropy can include this header from user space
 * to obtain the ioctl numbers exposed by the module.
 */

#ifndef _UAPI_JITTERENTROPY_KMOD_H
#define _UAPI_JITTERENTROPY_KMOD_H

#include <linux/types.h>
#include <linux/ioctl.h>

#ifndef __KERNEL__
# ifndef __user
#  define __user
# endif
#endif

/**
 * struct jent_kmod_status_ioc - argument for JENT_KMOD_IOC_STATUS
 * @buflen: in/out - size of @buf in bytes on input; on output, set to the
 *          number of bytes (excluding the trailing NUL) the kernel
 *          actually wrote into @buf. If @buflen on input is smaller than
 *          the JSON status payload the ioctl returns -ENOSPC; the caller
 *          may retry with a larger buffer.
 * @flags:  reserved, must be zero on input.
 * @buf:    user-space pointer to a buffer that receives the JSON status
 *          produced by jent_status(). Passing a __u64 (rather than a
 *          plain pointer) keeps the layout stable for 32-bit user space
 *          on a 64-bit kernel.
 *
 * The maximum @buflen accepted by the kernel is bounded
 * (JENT_KMOD_STATUS_MAX_BUFLEN) to keep one ioctl from allocating
 * arbitrarily large amounts of kernel memory.
 */
struct jent_kmod_status_ioc {
	__u32 buflen;
	__u32 flags;
	__u64 buf;
};

#define JENT_KMOD_STATUS_MAX_BUFLEN (64u * 1024u)

#define JENT_KMOD_IOC_MAGIC  'J'

/**
 * JENT_KMOD_IOC_STATUS - retrieve the jitterentropy status JSON
 *
 * Fills the user-supplied buffer with the JSON representation of the
 * current jitterentropy collector state (the same content
 * jent_status(3) produces). On success the on-input @buflen is updated
 * with the number of bytes written.
 */
#define JENT_KMOD_IOC_STATUS \
	_IOWR(JENT_KMOD_IOC_MAGIC, 0x01, struct jent_kmod_status_ioc)

#endif /* _UAPI_JITTERENTROPY_KMOD_H */
