# SPDX-License-Identifier: BSD-2-Clause
#
# macOS KEXT build for jitterentropy_kmod.
#
# Requires Xcode (or at least the Command Line Tools) so xcrun can
# locate the Kernel.framework headers and the appropriate SDK.
#
# Build with:
#   make                        # universal build for the host arch
#   make ARCH=arm64             # explicit arch
#   make ARCH=x86_64            # explicit arch
#   make load                   # build then kmutil load (needs root + reduced SIP)
#   make unload
#   make clean
#
# Targets:
#   build/jitterentropy_kmod.kext      The signed (or ad-hoc signed) bundle
#
# Apple killed third-party KEXTs starting in macOS 11. To load this:
#   - Intel: `csrutil disable` or `csrutil enable --without kext`
#   - Apple Silicon: boot into 1TR -> set "Reduced Security",
#                    allow "User Management of Kernel Extensions"
#   - Either: `kmutil load -p build/jitterentropy_kmod.kext`

XCRUN     ?= xcrun
SDK       ?= macosx
ARCH      ?= $(shell uname -m)
SDKROOT   := $(shell $(XCRUN) --sdk $(SDK) --show-sdk-path)
CC        := $(shell $(XCRUN) --sdk $(SDK) --find clang)
LD        := $(shell $(XCRUN) --sdk $(SDK) --find ld)
KEXTLIBS  := $(shell $(XCRUN) --sdk $(SDK) --find kextlibs)
CODESIGN  := codesign

JENT_TOP := ..
JENT_SRC := $(JENT_TOP)/src

# Kernel.framework lives at $SDKROOT/System/Library/Frameworks/Kernel.framework
KERN_INC := $(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers

# KEXT compile flags - mirror what Xcode emits for a "Generic Kernel
# Extension" target. -O0 is required by jitterentropy-base.c (it
# refuses to build under __OPTIMIZE__) and a no-op for our wrapper.
CFLAGS := -arch $(ARCH) \
          -isysroot $(SDKROOT) \
          -mkernel -nostdinc -fno-builtin -fno-common \
          -fno-stack-protector -fno-stack-check \
          -O0 -fwrapv -std=c11 \
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
          -DAPPLE -DNeXT \
          -I$(KERN_INC) \
          -I$(JENT_TOP) -I$(JENT_SRC) -I$(JENT_TOP)/arch

LDFLAGS := -arch $(ARCH) \
           -isysroot $(SDKROOT) \
           -Xlinker -kext -nostdlib \
           -lkmod -lkmodc++ -lcc_kext

OBJS := build/jitterentropy_kmod.o \
        build/jitterentropy-base.o \
        build/jitterentropy-gcd.o \
        build/jitterentropy-health.o \
        build/jitterentropy-noise.o \
        build/jitterentropy-sha3.o \
        build/jitterentropy-status.o \
        build/jitterentropy-timer.o

BUNDLE := build/jitterentropy_kmod.kext

.PHONY: all bundle sign clean load unload help

all: bundle

build:
	@mkdir -p build

build/jitterentropy_kmod.o: jitterentropy_kmod.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: $(JENT_SRC)/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/jitterentropy_kmod: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

bundle: build/jitterentropy_kmod Info.plist
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	cp Info.plist $(BUNDLE)/Contents/Info.plist
	cp build/jitterentropy_kmod $(BUNDLE)/Contents/MacOS/jitterentropy_kmod
	# Ad-hoc signing - good enough for development with reduced
	# security; production deployment needs an Apple-issued kext
	# signing entitlement.
	$(CODESIGN) --force --sign - $(BUNDLE) || true
	@echo "Built: $(BUNDLE)"

load: bundle
	sudo kmutil load -v 3 -p $(BUNDLE)

unload:
	sudo kmutil unload -b de.theil.jitterentropy_kmod || true

clean:
	rm -rf build

help:
	@echo "Targets: bundle (default), load, unload, clean"
	@echo "Variables:"
	@echo "  ARCH    target architecture (default: \$$(uname -m))"
	@echo "  SDK     xcrun SDK name (default: macosx)"
