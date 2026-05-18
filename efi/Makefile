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
CFLAGS := -O0 -fwrapv -ffreestanding -fno-stack-protector \
          -fno-stack-check -fshort-wchar -mno-red-zone \
          -fpic -fno-strict-aliasing -Wall -Wextra \
          -I$(EFI_INC) -I$(EFI_INC)/$(ARCH) -I$(EFI_INC)/protocol \
          -I$(JENT_TOP) -I$(JENT_SRC) -I$(JENT_TOP)/arch \
          -DEFI_FUNCTION_WRAPPER -DJENT_BAREMETAL

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

build/jitterentropy.efi: build/jitterentropy.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* \
	           -j .reloc \
	           -I $(ELF_TARGET) -O $(PEI_TARGET) \
	           --subsystem=10 $< $@

clean:
	rm -rf build
