// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-or-later
/*
 * Tiny UEFI application that brings up the jitterentropy library in
 * baremetal mode, reads 32 random bytes, and prints them as hex to the
 * EFI ConOut console.
 *
 * Built with gnu-efi. The jitterentropy sources are compiled in
 * directly with -DJENT_BAREMETAL so that all libc dependencies are
 * stubbed out, and the EFI boot-services AllocatePool / FreePool are
 * wired in as the baremetal allocator.
 */

#include <efi.h>
#include <efilib.h>

#include "jitterentropy.h"

/*
 * The library's baremetal mode declares memcpy/memset as required
 * symbols. gnu-efi's libefi.a already provides ABI-compatible
 * implementations (efi/lib/init.c), so we just use those.
 *
 * gcc may legitimately call memcmp at -O0 even without an explicit
 * source reference; provide a minimal one to avoid a link error.
 */
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	while (n--) {
		if (*p != *q)
			return (int)*p - (int)*q;
		p++; q++;
	}
	return 0;
}

/* --- Allocator hooks backed by EFI boot services ------------------------- */

static EFI_BOOT_SERVICES *g_bs;

static void *jent_efi_alloc(size_t len)
{
	void *out = NULL;
	EFI_STATUS s;

	if (!g_bs)
		return NULL;
	s = g_bs->AllocatePool(EfiLoaderData, len, &out);
	if (EFI_ERROR(s))
		return NULL;
	/* Zero - the library expects zeroed memory. */
	memset(out, 0, len);
	return out;
}

static void jent_efi_free(void *ptr, size_t len)
{
	(void)len;
	if (ptr && g_bs)
		g_bs->FreePool(ptr);
}

/* --- Helpers ------------------------------------------------------------- */

static void print_w(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
	st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}

static void print_hex_byte(EFI_SYSTEM_TABLE *st, unsigned char b)
{
	static const CHAR16 hex[] = u"0123456789abcdef";
	CHAR16 buf[3];

	buf[0] = hex[(b >> 4) & 0xf];
	buf[1] = hex[b & 0xf];
	buf[2] = 0;
	print_w(st, buf);
}

static void print_dec(EFI_SYSTEM_TABLE *st, int v)
{
	CHAR16 buf[12];
	int i = 0, neg = 0;

	if (v < 0) { neg = 1; v = -v; }
	if (v == 0) {
		buf[i++] = u'0';
	} else {
		while (v) {
			buf[i++] = (CHAR16)(u'0' + (v % 10));
			v /= 10;
		}
	}
	if (neg)
		buf[i++] = u'-';
	buf[i] = 0;
	/* reverse */
	for (int j = 0; j < i / 2; j++) {
		CHAR16 t = buf[j];
		buf[j] = buf[i - 1 - j];
		buf[i - 1 - j] = t;
	}
	print_w(st, buf);
}

/* --- EFI entry point ----------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	struct rand_data *ec;
	unsigned char out[32];
	ssize_t got;
	int rc;

	InitializeLib(image, st);
	g_bs = st->BootServices;

	print_w(st, u"jitterentropy EFI demo (library v");
	print_dec(st, (int)jent_version());
	print_w(st, u")\r\n");

	/* Register the EFI allocator with the baremetal library. */
	jent_baremetal_set_allocator(jent_efi_alloc, jent_efi_free);

	rc = jent_entropy_init_ex(0, 0);
	if (rc) {
		print_w(st, u"jent_entropy_init_ex failed: ");
		print_dec(st, rc);
		print_w(st, u"\r\n");
		return EFI_DEVICE_ERROR;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		print_w(st, u"jent_entropy_collector_alloc returned NULL\r\n");
		return EFI_OUT_OF_RESOURCES;
	}

	got = jent_read_entropy_safe(&ec, (char *)out, sizeof(out));
	if (got != (ssize_t)sizeof(out)) {
		print_w(st, u"jent_read_entropy_safe failed: ");
		print_dec(st, (int)got);
		print_w(st, u"\r\n");
		jent_entropy_collector_free(ec);
		return EFI_DEVICE_ERROR;
	}

	print_w(st, u"32 random bytes: ");
	for (size_t i = 0; i < sizeof(out); i++)
		print_hex_byte(st, out[i]);
	print_w(st, u"\r\n");

	jent_entropy_collector_free(ec);
	return EFI_SUCCESS;
}
