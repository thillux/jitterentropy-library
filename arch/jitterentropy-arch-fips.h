/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * Crypto-library / OS-specific FIPS-mode detection.
 *
 * Provides jent_fips_enabled() returning non-zero when the runtime is in
 * FIPS mode and zero otherwise. Dispatch:
 *   - LIBGCRYPT  -> gcry_fips_mode_active()
 *   - AWSLC      -> FIPS_mode()
 *   - OPENSSL    -> EVP_default_properties_is_fips_enabled(NULL)
 *   - Windows    -> 0 (no kernel switch to query)
 *   - POSIX      -> read /proc/sys/crypto/fips_enabled (Linux); on systems
 *                   that lack the file the open() fails and we report 0.
 *
 * The crypto-library branches expect the consumer to have included the
 * relevant headers before this one (gcrypt.h / openssl/evp.h).
 */

#ifndef _JITTERENTROPY_ARCH_FIPS_H
#define _JITTERENTROPY_ARCH_FIPS_H

#if defined(JENT_LINUX_KERNEL)
# include <linux/fips.h>
/*
 * <linux/fips.h> exposes fips_enabled either as an int (CONFIG_CRYPTO_FIPS=y)
 * or as a 0-valued preprocessor macro (CONFIG_CRYPTO_FIPS=n). struct rand_data
 * has a fips_enabled bitfield, so the macro form clashes: any later
 * "ec->fips_enabled = 1" expands to "ec->0 = 1" and fails to compile.
 *
 * Capture the current expansion through an inline accessor, then undefine
 * the macro so the field reference survives in the translation units that
 * pull this header transitively.
 */
static inline int jent_linux_kernel_fips_enabled(void)
{
	return fips_enabled;
}
# undef fips_enabled
#elif defined(JENT_FREEBSD_KERNEL)
/* No first-class FIPS-mode switch in the FreeBSD kernel. */
#elif defined(JENT_MACOS_KERNEL)
/* No third-party-queryable FIPS mode switch in xnu. */
#elif defined(JENT_BAREMETAL)
/* No FIPS mode switch on baremetal / EFI. */
#elif !defined(LIBGCRYPT) && !defined(AWSLC) && !defined(OPENSSL) && \
      !defined(_MSC_VER) && !defined(__MINGW32__)
# include <errno.h>
# include <fcntl.h>
# include <sys/types.h>
# include <unistd.h>
#endif

static inline int jent_fips_enabled(void)
{
#if defined(JENT_LINUX_KERNEL)
	return jent_linux_kernel_fips_enabled();
#elif defined(JENT_FREEBSD_KERNEL)
	return 0;
#elif defined(JENT_MACOS_KERNEL)
	return 0;
#elif defined(JENT_BAREMETAL)
	return 0;
#elif defined(LIBGCRYPT)
	return gcry_fips_mode_active();
#elif defined(AWSLC)
	return FIPS_mode();
#elif defined(OPENSSL)
	return EVP_default_properties_is_fips_enabled(NULL);
#elif defined(_MSC_VER) || defined(__MINGW32__)
	return 0;
#else
#define FIPS_MODE_SWITCH_FILE "/proc/sys/crypto/fips_enabled"
	char buf[2] = "0";
	int fd = 0;
	ssize_t rlen;

	if ((fd = open(FIPS_MODE_SWITCH_FILE, O_RDONLY)) >= 0) {
		do {
			rlen = read(fd, buf, sizeof(buf));
		} while (rlen < 0 && errno == EINTR);
		close(fd);
		if (rlen <= 0)
			return 0;
	}
	if (buf[0] == '1')
		return 1;
	else
		return 0;
#undef FIPS_MODE_SWITCH_FILE
#endif
}

#endif /* _JITTERENTROPY_ARCH_FIPS_H */
