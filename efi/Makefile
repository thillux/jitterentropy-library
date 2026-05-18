# SPDX-License-Identifier: BSD-3-Clause
#
# Build a tiny UEFI .efi binary that prints 32 jitterentropy bytes via
# the EFI ConOut console.
#
# Requires gnu-efi to be installed (Debian/Fedora: gnu-efi-devel).
#
#   make ARCH=x86_64                # default
#   make ARCH=aarch64
#
# Outputs:
#   build/jitterentropy.efi         # the bootable UEFI application

ARCH       ?= $(shell uname -m | sed -e 's/i.86/ia32/' -e 's/x86_64/x86_64/')
EFI_INC    ?= /usr/include/efi
EFI_LIB    ?= /usr/lib
EFI_CRT0   ?= $(EFI_LIB)/crt0-efi-$(ARCH).o
EFI_LDS    ?= $(EFI_LIB)/elf_$(ARCH)_efi.lds
EFI_LIBDIR ?= $(EFI_LIB)

JENT_TOP   := ..
JENT_SRC   := $(JENT_TOP)/src

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy

# -O0 is mandatory: jitterentropy-base.c refuses to compile under
# __OPTIMIZE__, and the jitter measurements would be invalidated by
# any optimisation pass.
#
# -DGNU_EFI_USE_MS_ABI forces gnu-efi's headers to tag every protocol
# function-pointer typedef with __attribute__((ms_abi)). Without this,
# direct calls into firmware would pass arguments in SysV registers
# (rdi, rsi, ...) while the firmware reads them from Microsoft x64
# registers (rcx, rdx, ...), which silently produces wrong output or
# hangs.
#
# We deliberately do NOT define EFI_FUNCTION_WRAPPER: the legacy
# uefi_call_wrapper(func, n, ...) macro is still available either way
# when needed (e.g. for AllocatePool), but the protocol typedefs being
# EFIAPI-tagged is what makes the direct-call path safe.
CFLAGS := -O0 -fwrapv -ffreestanding -fno-stack-protector \
          -fno-stack-check -fshort-wchar -mno-red-zone \
          -fpic -fno-strict-aliasing -Wall -Wextra \
          -maccumulate-outgoing-args \
          -I$(EFI_INC) -I$(EFI_INC)/$(ARCH) -I$(EFI_INC)/protocol \
          -I$(JENT_TOP) -I$(JENT_SRC) -I$(JENT_TOP)/arch \
          -DGNU_EFI_USE_MS_ABI -DJENT_BAREMETAL

#
# We invoke ld directly (rather than going through gcc) for the final
# link. Going through gcc pulls in PIE defaults / spec rewrites that
# add a second copy of crt0 and intercept the relocation flags. ld(1)
# accepts -z notext for the PC32-against-ImageBase relocation that the
# gnu-efi crt0 emits.
#
# --no-warn-rwx-segments silences a warning about EFI text+data sharing
# segments; that is by design in PE images.
LDFLAGS := -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
           -Bsymbolic -znoexecstack -znotext --no-warn-rwx-segments \
           -L$(EFI_LIBDIR) $(EFI_CRT0)

OBJS := build/main.o \
        build/jitterentropy-base.o \
        build/jitterentropy-gcd.o \
        build/jitterentropy-health.o \
        build/jitterentropy-noise.o \
        build/jitterentropy-sha3.o \
        build/jitterentropy-status.o \
        build/jitterentropy-timer.o

.PHONY: all clean
all: build/jitterentropy.efi

build:
	@mkdir -p build

build/main.o: main.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: $(JENT_SRC)/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/jitterentropy.so: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) -lefi -lgnuefi $(shell $(CC) -print-libgcc-file-name)

# Convert the shared object emitted by ld into a PE/COFF .efi binary
# that an EFI firmware can load.
#
# Recent binutils dropped the "efi-app-x86_64" target alias; the target
# is now spelled "pei-x86-64". Map the common arch names accordingly.
ifeq ($(ARCH),x86_64)
PEI_TARGET := pei-x86-64
else ifeq ($(ARCH),ia32)
PEI_TARGET := pei-i386
else ifeq ($(ARCH),aarch64)
PEI_TARGET := pei-aarch64-little
else
PEI_TARGET := pei-$(ARCH)
endif

ifeq ($(ARCH),x86_64)
ELF_TARGET := elf64-x86-64
else ifeq ($(ARCH),ia32)
ELF_TARGET := elf32-i386
else ifeq ($(ARCH),aarch64)
ELF_TARGET := elf64-littleaarch64
else
ELF_TARGET := elf64-little
endif

#
# Section selection MATTERS: anything not listed here is silently
# stripped, and the firmware will then run code with dangling pointers.
# In particular gnu-efi's LDS keeps the linker's .rodata, .dynstr,
# .eh_frame etc. in separate sections, so the obvious -j .text/.data
# list is not enough:
#   - .rodata holds every L"..." string literal (omitting it leaves
#     Print() pointing at unmapped memory and produces a silent EFI
#     application that does nothing at all).
#   - .dynstr backs .dynsym; gnu-efi's _relocate walks it during
#     startup self-relocation.
#
build/jitterentropy.efi: build/jitterentropy.so
	$(OBJCOPY) -j .text -j .sdata -j .data    \
	           -j .rodata -j .rodata.*        \
	           -j .dynamic -j .dynsym -j .dynstr \
	           -j .rel -j .rela -j .rel.* -j .rela.* \
	           -j .reloc                      \
	           -I $(ELF_TARGET) -O $(PEI_TARGET) \
	           --subsystem=10 $< $@

clean:
	rm -rf build
