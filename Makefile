# Compile Noise Source as user space application

CC ?= gcc
#Hardening
ENABLE_STACK_PROTECTOR ?= 1
# Only preferences in the default, which a CFLAGS from the environment or the
# make command line replaces. What the build is not correct without is appended
# below whatever CFLAGS says: -O0 (see the #error in src/jitterentropy-base.c),
# -fvisibility=hidden, without which every internal function is exported from
# the shared library - on the ELF linkers version.lds still limits the export
# set, on macOS nothing does - and further down -pthread, the internal timer
# and the include paths.
#
# Hence "override" on every append to CFLAGS and LDFLAGS. A plain += is
# ignored for a variable set on the command line, so "make CFLAGS=-g" used to
# build without any of them. And once a variable has an override, make ignores
# every later assignment to it that lacks one, so all of them carry it.
CFLAGS ?= --param ssp-buffer-size=4 -fPIE -Wextra -Wall -pedantic -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum
override CFLAGS +=-fPIC -O0 -fwrapv -fvisibility=hidden -std=c11

# -pthread rather than -lpthread: it is the spelling every supported toolchain
# understands, and on FreeBSD it is the only correct one (the library to link
# is libthr, which -pthread selects). It belongs in both the compile and the
# link step.
override CFLAGS +=-pthread
override LDFLAGS +=-pthread

UNAME_S := $(shell uname -s)

# Enable internal timer support
override CFLAGS += -DJENT_CONF_ENABLE_INTERNAL_TIMER

# Haiku maps the POSIX errno names onto its own B_* error codes, which are
# negative (based at INT_MIN), so the "return -EXXX" convention this library
# reports failure with comes out inverted: -ENOENT is a positive number there
# and every "if (ret < 0)" reads the failure as success. B_USE_POSITIVE_POSIX_ERRORS
# is Haiku's switch for POSIX-convention code and restores the usual positive
# errno values. See the fuller explanation in CMakeLists.txt.
ifeq ($(UNAME_S),Haiku)
override CFLAGS += -DB_USE_POSITIVE_POSIX_ERRORS
endif

GCCVERSIONFORMAT := $(shell echo `$(CC) -dumpversion | tr '.' '\n' | wc -l`)
ifeq "$(GCCVERSIONFORMAT)" "3"
  GCC_GTEQ_490 := $(shell expr `$(CC) -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/'` \>= 40900)
else
  GCC_GTEQ_490 := $(shell expr `$(CC) -dumpfullversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/'` \>= 40900)
endif

ifeq "$(ENABLE_STACK_PROTECTOR)" "1"
  ifeq "$(GCC_GTEQ_490)" "1"
    SSP_FLAG := -fstack-protector-strong
  else
    SSP_FLAG := -fstack-protector-all
  endif
  # Something has to define the __stack_chk_fail and __stack_chk_guard that
  # flag emits references to. Most C libraries do, so this costs nothing on
  # Linux, the BSDs, macOS, Cygwin and Android; Solaris' libc defines neither
  # and the runtime has to come from GCC's own libssp, without which every link
  # of an instrumented program fails with "ld: fatal: symbol referencing
  # errors"; Haiku has neither and cannot honour the flag at all.
  #
  # Hence the flag goes into LDFLAGS as well as CFLAGS: the driver links its
  # SSP runtime when it sees -fstack-protector* while driving the link, and
  # nothing when it does not, so naming -lssp here would hardcode one
  # platform's spelling of a choice the driver already makes correctly.
  #
  # Probed rather than keyed on UNAME_S, because illumos added both symbols to
  # its libc (illumos issue #5788) while still reporting SunOS, so the name
  # cannot separate the two. The probe compiles and links in one driver call
  # with the flag on it, which is the arrangement used below. It needs a local
  # array: the -strong variant only instruments frames that have one, and an
  # empty main() would link anywhere and settle nothing.
  SSP_USABLE := $(shell printf 'int main(int c,char**v){char b[64];(void)v;b[0]=(char)c;return b[0];}' \
	| $(CC) $(SSP_FLAG) -x c - -o /dev/null > /dev/null 2>&1 && echo yes)
  ifeq "$(SSP_USABLE)" "yes"
    override CFLAGS += $(SSP_FLAG)
    override LDFLAGS += $(SSP_FLAG)
  else
    $(warning Building WITHOUT $(SSP_FLAG): this toolchain resolves neither \
	__stack_chk_fail nor __stack_chk_guard, so the flag would break every link)
  endif
endif

# Change as necessary
PREFIX := /usr/local
# library target directory (either lib or lib64)
LIBDIR := lib

# include target directory
INCDIR := include
SRCDIR := src

NAME := jitterentropy
# sed rather than awk: minimal container images (SLE BCI) ship no awk.
#
# Only spaces and basic regex: no \s (GNU, and OpenBSD reads it as 's') and no
# [[:space:]] (the legacy Solaris /usr/bin/sed does not know the classes).
JENT_VERSION_SED = sed -n 's/^\#define  *$(1)  *\([0-9][0-9]*\).*/\1/p' jitterentropy.h
LIBMAJOR=$(shell $(call JENT_VERSION_SED,JENT_MAJVERSION))
LIBMINOR=$(shell $(call JENT_VERSION_SED,JENT_MINVERSION))
LIBPATCH=$(shell $(call JENT_VERSION_SED,JENT_PATCHLEVEL))
LIBVERSION := $(LIBMAJOR).$(LIBMINOR).$(LIBPATCH)

# A version component that did not parse would otherwise be found only in the
# installed file name. Fail the build instead.
ifeq ($(strip $(LIBMAJOR)),)
$(error could not read JENT_MAJVERSION from jitterentropy.h)
endif
ifeq ($(strip $(LIBMINOR)),)
$(error could not read JENT_MINVERSION from jitterentropy.h)
endif
ifeq ($(strip $(LIBPATCH)),)
$(error could not read JENT_PATCHLEVEL from jitterentropy.h)
endif

ARCHDIR := arch
# No VPATH: it would also find the objects the kernel build leaves beside the
# sources in src/ and arch/ and take them for this build's. The explicit rules
# below look only at the sources.
C_SRCS := $(notdir $(sort $(wildcard $(SRCDIR)/*.c) $(wildcard $(ARCHDIR)/*.c)))
C_OBJS := ${C_SRCS:.c=.o}
OBJS := $(C_OBJS)

analyze_src_plists = $(patsubst $(SRCDIR)/%.c,%.plist,$(wildcard $(SRCDIR)/*.c))
analyze_arch_plists = $(patsubst $(ARCHDIR)/%.c,%.plist,$(wildcard $(ARCHDIR)/*.c))
analyze_plists = $(analyze_src_plists) $(analyze_arch_plists)

INCLUDE_DIRS := . $(SRCDIR)
LIBRARY_DIRS :=

# Shared-library naming and hardening flags are toolchain specific. Apple's
# ld64 understands neither -z relro/now nor -soname, macOS has no librt (the
# POSIX timer/clock functions live in libSystem), and shared libraries are
# .dylib carrying an install name rather than .so carrying an soname.
ifeq ($(UNAME_S),Darwin)
LIBRARIES :=
SOEXT := dylib
SONAME := lib$(NAME).$(LIBMAJOR).$(SOEXT)
SOFILE := lib$(NAME).$(LIBVERSION).$(SOEXT)
SONAME_FLAGS = -install_name $(PREFIX)/$(LIBDIR)/$(SONAME) \
	-current_version $(LIBVERSION) -compatibility_version $(LIBMAJOR)
# Apple's strip(1) refuses a full strip of a dylib (the exported symbols
# must remain), so "install -s" aborts the install; install unstripped and
# remove only the local symbols afterwards.
INSTALL_STRIP ?= install
STRIP_SHARED := strip -x
else
# librt is a separate library only on Linux (glibc before 2.17) and Solaris.
# On the BSDs the POSIX clock and timer functions live in libc, and OpenBSD
# ships no librt at all - naming it unconditionally made the link fail there
# with "cannot find -lrt".
ifneq (,$(filter $(UNAME_S),Linux SunOS))
LIBRARIES := rt
else
LIBRARIES :=
endif
SOEXT := so
SONAME := lib$(NAME).$(SOEXT).$(LIBMAJOR)
SOFILE := lib$(NAME).$(SOEXT).$(LIBVERSION)
SONAME_FLAGS = -Wl,-soname,$(SONAME)
# -z relro / -z now are GNU ld and lld spellings. Apple's ld64 is handled by
# the Darwin branch above; the Solaris link editor takes neither in this form,
# so the hardening is applied only where it is known to be understood.
#
# --version-script is the same set of linkers, and it is what limits the
# shared library to the API of jitterentropy.h (see version.lds). Only the
# shared link takes it: an archive has no dynamic symbol table to restrict.
ifneq (,$(filter $(UNAME_S),Linux FreeBSD OpenBSD NetBSD DragonFly))
override LDFLAGS += -Wl,-z,relro,-z,now
VERSION_SCRIPT := version.lds
SO_LDFLAGS += -Wl,--version-script=$(VERSION_SCRIPT)
endif
INSTALL_STRIP ?= install -s
STRIP_SHARED := :
endif
SOLINK := lib$(NAME).$(SOEXT)

override CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
override LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
override LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

# The headers each object was compiled from, written by the compiler beside the
# object as it compiles (-MMD), so that editing a header rebuilds what includes
# it. -MP adds an empty rule per header, which keeps a deleted or renamed
# header from stopping the build with "No rule to make target". Only CPPFLAGS
# gets these, not CFLAGS: the scan and clang --analyze rules below pass CFLAGS
# and would otherwise write .d files for the .plist targets.
override CPPFLAGS += -MMD -MP
DEPS := $(C_OBJS:.o=.d)

.PHONY: all scan install clean distclean check $(NAME) $(NAME)-static

all: $(NAME) $(NAME)-static

# The suites whose Makefiles can run them. Each absorbs the sources it tests,
# so none needs the library built first. The CTest suite is the complete one
# (ctest -LE unreliable in a CMake build tree); this is the fallback for a tree
# without CMake, as the Makefiles under tests/ are. The flags this Makefile
# built up are not handed down - they name include paths relative to this
# directory - and each of those Makefiles sets its own. Variables from the make
# command line would reach them all the same, through MAKEFLAGS rather than the
# environment, and a CFLAGS there makes them drop their own appends; hence the
# empty MAKEOVERRIDES, with CC handed on by name as the one worth keeping.
check: MAKEOVERRIDES :=
check:
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/unit check CC="$(CC)"
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/gcd CC="$(CC)" && tests/gcd/gcd
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/health check CC="$(CC)"

lib$(NAME).a: $(OBJS)
	$(AR) rcs lib$(NAME).a $(OBJS)

$(SOFILE): $(OBJS) $(VERSION_SCRIPT)
	$(CC) -shared $(SONAME_FLAGS) -o $(SOFILE) $(OBJS) $(LDFLAGS) \
		$(SO_LDFLAGS)

$(NAME)-static: lib$(NAME).a
$(NAME): $(SOFILE)

# Compile rules naming the source directory; see the note at C_SRCS.
%.o: $(SRCDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: $(ARCHDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Absent before the first build, hence the "-".
-include $(DEPS)

$(analyze_src_plists): %.plist: $(SRCDIR)/%.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

$(analyze_arch_plists): %.plist: $(ARCHDIR)/%.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability $(shell find * -name \*.h -o -name \*.c)

install: install-man install-shared install-includes

install-man:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/share/man/man3
	install -m 0644 doc/$(NAME).3 $(DESTDIR)$(PREFIX)/share/man/man3/
	gzip -n -f -9 $(DESTDIR)$(PREFIX)/share/man/man3/$(NAME).3

install-shared:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	$(INSTALL_STRIP) -m 0755 $(SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	$(STRIP_SHARED) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SOFILE)
	$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SONAME)
	ln -sf $(SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SOLINK)

# jitterentropy.h is the whole installed interface. The arch/ headers are
# internal to the build: nothing the public header declares needs them - struct
# jent_notime_ctx is defined in jitterentropy.h itself, so it compiles on its
# own - and they declare functions that are not exported from the library.
# CMakeLists.txt installs the same set.
install-includes:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(INCDIR)
	install -m 0644 jitterentropy.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

# 0644, as CMake's install(TARGETS) gives an archive: it is data for the
# linker, not something that is executed, and 0755 on it is what rpmlint
# reports as spurious-executable-perm and Lintian as executable-not-elf-or-script.
# The shared library above keeps 0755 - that one is mapped executable, and the
# distributions expect the mode there.
install-static:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -m 0644 lib$(NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/

clean:
	@- $(RM) $(NAME)
	@- $(RM) $(OBJS) $(DEPS)
	@- $(RM) $(addprefix $(SRCDIR)/,$(C_OBJS)) $(addprefix $(ARCHDIR)/,$(C_OBJS))
	@- $(RM) lib$(NAME).so* lib$(NAME).*dylib
	@- $(RM) lib$(NAME).a
	@- $(RM) $(analyze_plists)

distclean: clean
