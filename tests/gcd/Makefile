#
# Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
#

CC		?= gcc
CFLAGS		+= -Wextra -Wall -pedantic -fPIC -O2 -std=c11
#Hardening
CFLAGS		+= -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -fPIE -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum

#Optimizations
CFLAGS		+= -flto
LDFLAGS		+= -flto

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDFLAGS		+= -Wl,-z,relro,-z,now,--as-needed -pie
endif

NAME		:= gcd

DESTDIR		:=
ETCDIR		:= /etc
BINDIR		:= /bin
SBINDIR		:= /sbin
SHAREDIR	:= /usr/share/keyutils
MANDIR		:= /usr/share/man
MAN1		:= $(MANDIR)/man1
MAN3		:= $(MANDIR)/man3
MAN5		:= $(MANDIR)/man5
MAN7		:= $(MANDIR)/man7
MAN8		:= $(MANDIR)/man8
INCLUDEDIR	:= /usr/include
LN		:= ln
LNS		:= $(LN) -sf
# Apple's strip(1) reads -s as "strip the symbols named in this file" and
# then takes the binary as that file, aborting with "no files specified".
# Plain strip removes the same symbols from an executable there.
ifeq ($(UNAME_S),Darwin)
STRIP		?= strip
else
STRIP		?= strip -s
endif

###############################################################################
#
# Define compilation options
#
###############################################################################
INCLUDE_DIRS	:= ../../ ../../src
LIBRARY_DIRS	:=
LIBRARIES	:=

CFLAGS		+= $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS		+= $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS		+= $(foreach library,$(LIBRARIES),-l$(library))

###############################################################################
#
# Get version name and cross check
#
###############################################################################
VERFILE    := ../../jitterentropy.h

APPMAJOR   := $(shell awk '$$2 == "JENT_MAJVERSION" {print $$3}' $(VERFILE))
APPMINOR   := $(shell awk '$$2 == "JENT_MINVERSION" {print $$3}' $(VERFILE))
APPPATCH   := $(shell awk '$$2 == "JENT_PATCHLEVEL" {print $$3}' $(VERFILE))
APPVERSION := $(APPMAJOR).$(APPMINOR).$(APPPATCH)

###############################################################################
#
# Define files to be compiled
#
###############################################################################
C_SRCS := $(wildcard *.c) ../../src/jitterentropy-gcd.c \
	  ../../arch/jitterentropy-arch-memory.c \
	  ../../arch/jitterentropy-arch-atomic.c
# Everything this builds lands here, not next to the sources it is built from.
# ${C_SRCS:.c=.o} kept the path of each source, so the three library sources
# above left their objects in ../../src and ../../arch - the very directories
# the root Makefile names in its VPATH. A plain `make` at the root then found
# them, took them for its own, skipped compiling those three and handed the
# linker names that exist only in the root directory:
#
#   cc: error: no such file or directory: 'jitterentropy-arch-atomic.o'
#
# The basenames are unique across the three directories, so stripping the
# paths is enough; the vpath below is what lets make find the sources again.
vpath %.c ../../src ../../arch

C_OBJS := $(notdir ${C_SRCS:.c=.o})
# From the objects, not the sources: gcov writes .gcno and .gcda beside the
# object file, which is here.
C_GCOV := ${C_OBJS:.o=.gcda}
C_GCOV += ${C_OBJS:.o=.gcno}
C_GCOV += ${C_OBJS:.o=.gcov}
OBJS := $(C_OBJS)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(notdir $(analyze_srcs:%.c=%.plist))

.PHONY: all scan install clean cppcheck distclean debug asanaddress asanthread leak gcov

all: $(NAME)

debug: CFLAGS += -g -DDEBUG
debug: DBG-$(NAME)

asanaddress: CFLAGS += -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer
asanaddress: LDFLAGS += -fsanitize=address
asanaddress: DBG-$(NAME)

asanthread: CFLAGS += -g -DDEBUG -fsanitize=thread -fno-omit-frame-pointer
asanthread: LDFLAGS += -fsanitize=thread
asanthread: DBG-$(NAME)

leak: CFLAGS += -g -DDEBUG -fsanitize=leak -fno-omit-frame-pointer
leak: LDFLAGS += -fsanitize=leak
leak: DBG-$(NAME)

# Compile for the use of GCOV
# Usage after compilation: gcov <file>.c
gcov: CFLAGS += -g -DDEBUG -fprofile-arcs -ftest-coverage
gcov: LDFLAGS += -fprofile-arcs
gcov: DBG-$(NAME)

###############################################################################
#
# Build the library
#
##############################################################################

$(NAME): $(OBJS)
	$(CC) -o $(NAME) $(OBJS) $(LDFLAGS)
	$(STRIP) $(NAME)

DBG-$(NAME): $(OBJS)
	$(CC) -g -DDEBUG -o $(NAME) $(OBJS) $(LDFLAGS)

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability *.h *.c ../lib/*.c ../lib/*.h

# Here as well - see the vpath above.
C_ASM := $(notdir ${C_SRCS:.c=.s})

asm:
	$(foreach b, $(C_SRCS), $(CC) $(CFLAGS) -S -fverbose-asm -o $(notdir ${b:.c=.s}) $(b);)

install:
	install -m 0755 $(NAME) -D -t $(DESTDIR)$(BINDIR)/

###############################################################################
#
# Clean
#
###############################################################################

clean:
	@- $(RM) $(OBJS)
	@- $(RM) $(NAME)
	@- $(RM) $(C_GCOV)
	@- $(RM) *.gcov
	@- $(RM) $(analyze_plists)
	@- $(RM) $(C_ASM)

distclean: clean

###############################################################################
#
# Show variables
#
###############################################################################
show_vars:
	@echo APPVERSION=$(APPVERSION)
	@echo DESTDIR=$(DESTDIR)
	@echo BINDIR=$(BINDIR)
	@echo NAME=$(NAME)
	@echo LDFLAGS=$(LDFLAGS)
	@echo CFLAGS=$(CFLAGS)
