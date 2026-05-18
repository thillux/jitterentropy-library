// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-or-later
/*
 * Tiny UEFI application that brings up the jitterentropy library in
 * baremetal mode, reads 32 random bytes, and prints them as hex to the
 * EFI ConOut console.
 *
 * Implementation notes:
 *
 *   - All console output goes through gnu-efi's Print() helper rather
 *     than poking ConOut->OutputString directly. Print() takes care of
 *     the Microsoft x64 calling convention that EFI firmware expects;
 *     a direct call would silently miscall the firmware if EFIAPI is
 *     not on the function-pointer typedef.
 *   - We use plain L"..." (wide string) literals together with
 *     -fshort-wchar so the encoding matches CHAR16. The literals end
 *     up in .rodata and get relocated at runtime via gnu-efi's
 *     _relocate.
 *   - jitterentropy is built with JENT_BAREMETAL; we wire EFI's
 *     AllocatePool / FreePool in as the baremetal allocator.
 */

#include <efi.h>
#include <efilib.h>

#include "jitterentropy.h"

/*
 * Baremetal mode declares memcpy / memset as required symbols. gnu-efi's
 * libefi.a already provides ABI-compatible implementations
 * (efi/lib/init.c), so we just use those. gcc may also call memcmp
 * implicitly at -O0; provide a minimal one.
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
	s = uefi_call_wrapper(g_bs->AllocatePool, 3,
			      EfiLoaderData, (UINTN)len, &out);
	if (EFI_ERROR(s))
		return NULL;
	/* Zero - the library expects zeroed memory. */
	for (size_t i = 0; i < len; i++)
		((unsigned char *)out)[i] = 0;
	return out;
}

static void jent_efi_free(void *ptr, size_t len)
{
	(void)len;
	if (ptr && g_bs)
		uefi_call_wrapper(g_bs->FreePool, 1, ptr);
}

/* --- EFI entry point ----------------------------------------------------- */

/*
 * Calling convention note: gnu-efi's crt0-efi-x86_64.S calls efi_main
 * with SysV registers (rdi = ImageHandle, rsi = SystemTable). The
 * crt0 itself bridges from the MS-ABI entry point that the PE loader
 * uses. Marking efi_main with EFIAPI (which is ms_abi) would mean we
 * try to read the arguments from rcx/rdx instead - those have been
 * clobbered by _relocate, so we'd silently run with garbage pointers
 * and ConOut->OutputString would never produce anything.
 *
 * Conclusion: efi_main must be plain SysV here, even though every EFI
 * tutorial that targets EDK2 puts EFIAPI on it. gnu-efi's crt0 is the
 * impedance matcher.
 */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	struct rand_data *ec;
	unsigned char out[32];
	ssize_t got;
	int rc;
	UINTN i;

	InitializeLib(image, st);
	g_bs = st->BootServices;

	Print(L"jitterentropy EFI demo (library v%u)\r\n",
	      (UINTN)jent_version());

	/* Register the EFI allocator with the baremetal library. */
	jent_baremetal_set_allocator(jent_efi_alloc, jent_efi_free);

	rc = jent_entropy_init_ex(0, 0);
	if (rc) {
		Print(L"jent_entropy_init_ex failed: %d\r\n", rc);
		return EFI_DEVICE_ERROR;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		Print(L"jent_entropy_collector_alloc returned NULL\r\n");
		return EFI_OUT_OF_RESOURCES;
	}

	got = jent_read_entropy_safe(&ec, (char *)out, sizeof(out));
	if (got != (ssize_t)sizeof(out)) {
		Print(L"jent_read_entropy_safe failed: %d\r\n", (int)got);
		jent_entropy_collector_free(ec);
		return EFI_DEVICE_ERROR;
	}

	Print(L"32 random bytes: ");
	for (i = 0; i < sizeof(out); i++)
		Print(L"%02x", (UINTN)out[i]);
	Print(L"\r\n");

	jent_entropy_collector_free(ec);
	return EFI_SUCCESS;
}
