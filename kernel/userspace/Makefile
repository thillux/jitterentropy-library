# SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
#
# Build the userspace status helper library and the jitterentropy-status tool.

CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=c11
# The shared UAPI header lives in the parent (kernel/) directory.
CFLAGS += -I..

TOOL := jitterentropy-status
LIB  := libjent_status.a

.PHONY: all clean

all: $(TOOL) $(LIB)

$(LIB): libjent_status.o
	$(AR) rcs $@ $^

$(TOOL): jent_status_tool.o libjent_status.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c libjent_status.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) *.o $(LIB) $(TOOL)
