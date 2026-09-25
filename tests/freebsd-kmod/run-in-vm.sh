#!/bin/sh
# Build tests/freebsd-kmod against the running kernel's sources, load it and
# check that it passed. What the freebsd-kmod job in ci.yml runs, as root, in
# a FreeBSD VM; see README.md in this directory.
#
# Strictly POSIX sh.

set -e

cd "$(dirname "$0")"

echo "==> $(uname -a)"

# The VM images do not all ship the kernel sources. Those of the running
# release are in its src.txz; a patch level (-pN) is not part of the path.
# The path names the machine and the machine architecture (arm64/aarch64,
# amd64/amd64), which uname -m and uname -p report.
SYSDIR=/usr/src/sys
if [ ! -f "$SYSDIR/conf/kmod.mk" ]; then
	rel=$(uname -r | sed 's/-p[0-9]*$//')
	url="https://download.freebsd.org/releases/$(uname -m)/$(uname -p)/$rel/src.txz"
	echo "==> $SYSDIR missing, fetching $url"
	fetch -o /tmp/src.txz "$url"
	tar -C / -xf /tmp/src.txz usr/src/sys
	rm -f /tmp/src.txz
fi

# BSD make, the system's own - not gmake. Built in an object directory of its
# own: in the source directory bsd.obj.mk warns "Object directory not changed
# from original", and the checkout stays free of build products. MAKEOBJDIR
# is only honoured from the environment and only if the directory exists.
MAKEOBJDIR=$(mktemp -d /tmp/jitterentropy-kmod.XXXXXX)
export MAKEOBJDIR
make SYSDIR="$SYSDIR"

# A failure in MOD_LOAD fails kldload; either way the module's own report is
# what explains it.
rc=0
kldload "$MAKEOBJDIR/jitterentropy_test.ko" || rc=$?
dmesg | sed -n '/jitterentropy_test: library version/,$p'
if [ "$rc" -ne 0 ]; then
	echo "kldload failed with $rc" >&2
	exit 1
fi

if ! dmesg | grep -q '^jitterentropy_test: PASS$'; then
	echo "no PASS line from the module" >&2
	exit 1
fi

kldunload jitterentropy_test
echo "==> jitterentropy_test loaded, passed and unloaded"
