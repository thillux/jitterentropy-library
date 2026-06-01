/*
 * Small GNU-EFI demo for the jitterentropy library.
 *
 * Initialises the noise source, allocates an entropy collector, reads
 * 32 random bytes and prints them as hex over the EFI console. Useful
 * as a smoke test that the library compiles and works in a freestanding
 * (pre-OS) environment on the current firmware.
 *
 * Build with Makefile.efi (out-of-tree) -- this binary expects to be
 * loaded by UEFI firmware and called once; it returns to the firmware
 * boot manager after printing the random bytes.
 */

#include <efi.h>
#include <efilib.h>

#include "jitterentropy.h"

static void
print_hex(const unsigned char *buf, UINTN len)
{
	UINTN i;

	for (i = 0; i < len; i++)
		Print(L"%02x", buf[i]);
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
	struct rand_data *ec;
	unsigned char buf[32];
	ssize_t got;
	int ret;

	InitializeLib(image_handle, system_table);

	Print(L"jitterentropy-efi: library version %u\n", jent_version());

	ret = jent_entropy_init();
	if (ret != 0) {
		Print(L"jent_entropy_init failed: %d\n", ret);
		return EFI_DEVICE_ERROR;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (ec == NULL) {
		Print(L"jent_entropy_collector_alloc failed\n");
		return EFI_OUT_OF_RESOURCES;
	}

	got = jent_read_entropy_safe(&ec, (char *)buf, sizeof(buf));
	if (got != (ssize_t)sizeof(buf)) {
		Print(L"jent_read_entropy_safe returned %ld\n", (long)got);
		jent_entropy_collector_free(ec);
		return EFI_DEVICE_ERROR;
	}

	Print(L"random: ");
	print_hex(buf, sizeof(buf));
	Print(L"\n");

	jent_entropy_collector_free(ec);
	return EFI_SUCCESS;
}
