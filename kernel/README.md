# Jitter RNG Linux kernel module

This directory contains an out-of-tree Linux kernel module that exposes the
jitterentropy library through character devices and a hwrng backend, plus a
small userspace helper to query the device status.

## What it provides

- **`/dev/jitterentropy`** — a character device backed by a single,
  module-global Jitter RNG instance shared by all readers. Access to the
  instance is serialized with a mutex.
- **`/dev/jitterentropy-multi`** — a character device that allocates an
  independent Jitter RNG instance for every open file descriptor and frees it
  when the descriptor is closed. Use this when concurrent, independent readers
  are required (a single `struct rand_data` must not be used by more than one
  thread at a time).
- **hwrng backend** named `jitterentropy`, registered with the kernel
  `hw_random` framework (visible via `/dev/hwrng` and
  `/sys/class/misc/hw_random/`). It uses its own dedicated instance.
- **`JENT_IOC_STATUS` ioctl** — returns the JSON status (the output of
  `jent_status()`) of the Jitter RNG instance backing the file descriptor the
  ioctl is issued on. The ABI is defined in `jitterentropy_uapi.h`.

## Module parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `osr`     | `1`     | Oversampling rate passed to `jent_entropy_collector_alloc()`. |
| `flags`   | `0`     | `JENT_*` flags bitmask (see `jitterentropy.h`). `JENT_FORCE_INTERNAL_TIMER` is rejected — see below. |
| `quality` | `0`     | hwrng entropy quality in bits per 1024 bits of output. `0` registers the hwrng without feeding the kernel entropy pool automatically. |

Example:

```sh
sudo insmod jitterentropy_drv.ko osr=3 flags=0 quality=512
```

## Timer / noise source

This module is built **without** the internal timer thread
(`JENT_CONF_ENABLE_INTERNAL_TIMER` is not defined), so it relies solely on the
hardware time stamp noise source (`random_get_entropy()`, i.e. RDTSC on x86,
the cycle/virtual counter on arm64, ...). Requesting
`JENT_FORCE_INTERNAL_TIMER` via `flags` is therefore rejected at load time, and
`JENT_DISABLE_INTERNAL_TIMER` is forced on.

## Building

From the repository root:

```sh
make -f Makefile.kernel              # build the module and the userspace helper
make -f Makefile.kernel modules      # only the kernel module
make -f Makefile.kernel userspace    # only the userspace helper
make -f Makefile.kernel clean
```

Build against a specific kernel tree with `KDIR`:

```sh
make -f Makefile.kernel KDIR=/path/to/linux
```

The library sources in `../src` are compiled into the module via thin
`jent_lib_*.c` wrappers so the upstream sources stay untouched. They are built
with `-O0` as the Jitter RNG requires (the compiler must not optimize away the
timing variations the noise source depends on).

Both 64-bit and 32-bit kernels are supported. The handful of 64-bit divisions
in the library (the timer GCD reduction) are routed through the kernel's
`div64_u64()` on kernel builds via the `jent_div64()` / `jent_mod64()` helpers
in `src/jitterentropy-internal.h`, so no `__udivdi3`/`__umoddi3` libgcc helpers
are referenced. These divisions run outside the timed measurement region and
therefore do not affect entropy collection.

## Userspace status helper

`userspace/` contains a tiny library (`libjent_status`) and a CLI tool
(`jitterentropy-status`) that open a device, issue `JENT_IOC_STATUS`, and print
the JSON document:

```sh
./kernel/userspace/jitterentropy-status                 # /dev/jitterentropy
./kernel/userspace/jitterentropy-status /dev/jitterentropy-multi
```

The status JSON can be validated/queried with `jq`, e.g.:

```sh
./kernel/userspace/jitterentropy-status | jq .
```

## In-kernel cache-size detection

The memory-access noise source is sized from the CPU data/unified cache size.
In the kernel module this is read on **x86** directly via CPUID leaf 4 (using
the module-safe `cpuid_count()` helper). On **arm/arm64 and other
architectures** the module falls back to the library's built-in default size:
the generic `cacheinfo` accessor (`get_cpu_cacheinfo()`) is not exported to
modules, and reading the cache-geometry system registers from a module is too
fragile to do portably.

## Testing with Nix

A flake at the repository root builds and tests everything:

```sh
nix build .#kernel-module             # build the .ko against nixpkgs' kernel
nix build .#jitterentropy             # core library (glibc) + runtime smoke test
nix build .#jitterentropy-musl        # core library (musl) + runtime smoke test
nix build .#jitterentropy-cmake       # core library + tools via the CMake build
nix build .#jitterentropy-openssl     # external crypto backend: OpenSSL
nix build .#jitterentropy-gcrypt      # external crypto backend: libgcrypt
nix build .#jitterentropy-awslc       # external crypto backend: AWS-LC
nix build .#jitterentropy-mingw       # Windows cross build (mingw-w64)
nix build .#jitterentropy-status      # userspace status tool/library
nix flake check                       # all of the above + the NixOS VM test
```

`nix flake check` runs `nix/vm-test.nix`, a NixOS VM test that boots a machine
with the module auto-loaded (and all userspace tools in
`environment.systemPackages`), then exercises both character devices, the
status ioctl and the hwrng backend. The same builds run in CI (see
`.github/workflows/ci.yml`).
