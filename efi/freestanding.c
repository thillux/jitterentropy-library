/*
 * Freestanding compatibility shims for the jitterentropy library when
 * linked into a GNU-EFI binary.
 *
 * arch/jitterentropy-arch-memory.h's baremetal arm declares jent_zalloc
 * / jent_zfree / jent_memset_secure as externs; back them here with
 * EFI boot-services AllocatePool / FreePool / ZeroMem.
 *
 * memcpy / memset / memcmp are already supplied by libefi.a's init.o,
 * so we deliberately do NOT redefine them here -- linking that file
 * pulls in their implementations.
 */

#include <efi.h>
#include <efilib.h>

#include <stddef.h>
#include <stdint.h>

void *
jent_zalloc(size_t len)
{
	void *p = NULL;
	EFI_STATUS s;

	if (len == 0)
		return NULL;
	s = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, len, &p);
	if (EFI_ERROR(s) || p == NULL)
		return NULL;
	ZeroMem(p, len);
	return p;
}

void
jent_zfree(void *ptr, size_t len)
{
	if (ptr == NULL)
		return;
	ZeroMem(ptr, len);
	(void)uefi_call_wrapper(BS->FreePool, 1, ptr);
}

void
jent_memset_secure(void *s, size_t n)
{
	ZeroMem(s, n);
}

/*
 * gnu-efi's libefi.a ships memcpy and memset (we leave those to it,
 * otherwise the linker errors on duplicate symbols). It does NOT ship
 * memmove, memcmp, or strlen, which the library nevertheless needs --
 * provide trivial implementations here.
 */
int
memcmp(const void *a, const void *b, size_t n)
{
	return (int)CompareMem((void *)a, (void *)b, n);
}

void *
memmove(void *dst, const void *src, size_t n)
{
	CopyMem(dst, (void *)src, n);
	return dst;
}

size_t
strlen(const char *s)
{
	size_t n = 0;

	while (*s++ != '\0')
		n++;
	return n;
}
