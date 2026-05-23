{
  description = "CPU Jitter RNG library, Linux kernel module and userspace tools";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f system nixpkgs.legacyPackages.${system});

      version = "3.7.1";
      src = self;

      # Core userspace library, built and installed via the upstream Makefile.
      # The check phase additionally builds and runs a small smoke test so the
      # library is exercised at runtime against the selected C library
      # (glibc / musl). JENT_CONF_RELAX_MLOCK keeps the test working inside the
      # Nix build sandbox, where mlock() is typically not permitted.
      mkCoreLib = stdenv: stdenv.mkDerivation {
        pname = "jitterentropy";
        inherit version src;
        enableParallelBuilding = true;
        hardeningDisable = [ "fortify" ]; # _FORTIFY_SOURCE is inert at -O0

        installPhase = ''
          runHook preInstall
          make install install-static PREFIX=$out
          runHook postInstall
        '';

        doCheck = true;
        checkPhase = ''
          runHook preCheck
          echo "Building and running the jitterentropy smoke test ..."
          $CC -O0 -std=gnu11 -D_GNU_SOURCE \
              -DJENT_CONF_ENABLE_INTERNAL_TIMER -DJENT_CONF_RELAX_MLOCK \
              -I. -Isrc -pthread \
              src/*.c nix/smoketest.c -o smoketest
          ./smoketest
          runHook postCheck
        '';
      };

      # Core userspace library built with the CMake build system instead of the
      # Makefile. This also builds and installs the recording/validation tools
      # (jitterentropy-rng, jitterentropy-osr, jitterentropy-hashtime, gcd,
      # extractlsb) which is why it is convenient to drop into a system's PATH.
      mkCmakeLib = pkgs: stdenv: stdenv.mkDerivation {
        pname = "jitterentropy-cmake";
        inherit version src;
        nativeBuildInputs = [ pkgs.cmake ];
        # The library translation units are compiled with -O0 by the project's
        # own flags; _FORTIFY_SOURCE is inert there and would only warn.
        hardeningDisable = [ "fortify" ];
        # Build the shared library in addition to the tools.
        cmakeFlags = [ "-DBUILD_SHARED_LIBS=ON" ];
      };

      # Core library built with the CMake build system against an external
      # crypto backend (OpenSSL / libgcrypt / AWS-LC) for the SHA-3 conditioner,
      # secure memory and FIPS-mode detection. The library headers select the
      # backend from the -D${backend} define that CMake adds for EXTERNAL_CRYPTO;
      # the include/library locations are pinned explicitly because the project
      # discovers them with find_path/find_library, which needs help in the Nix
      # store. Building exercises the backend's compile and link paths.
      mkCryptoLib = pkgs: backend: { buildInputs, includeDir, libraryFile }:
        pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-" + pkgs.lib.toLower backend;
          inherit version src;
          nativeBuildInputs = [ pkgs.cmake ];
          inherit buildInputs;
          hardeningDisable = [ "fortify" ];
          cmakeFlags = [
            "-DBUILD_SHARED_LIBS=ON"
            "-DEXTERNAL_CRYPTO=${backend}"
            "-DLIBCRYPTO_INCLUDE_DIR=${includeDir}"
            "-DLIBCRYPTO_LIBRARY=${libraryFile}"
          ];
        };

      # Userspace status helper (library + CLI) for the kernel devices. Needs
      # the kernel UAPI headers (linux/ioctl.h) which come from linuxHeaders.
      mkStatusTool = pkgs: stdenv: stdenv.mkDerivation {
        pname = "jitterentropy-status";
        inherit version src;
        buildInputs = [ pkgs.linuxHeaders ];

        buildPhase = ''
          runHook preBuild
          make -C kernel/userspace
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/bin $out/lib $out/include
          install -m755 kernel/userspace/jitterentropy-status $out/bin/
          install -m644 kernel/userspace/libjent_status.a     $out/lib/
          install -m644 kernel/userspace/libjent_status.h     $out/include/
          install -m644 kernel/jitterentropy_uapi.h           $out/include/
          runHook postInstall
        '';
      };

      # Out-of-tree kernel module, built against a given kernel derivation.
      mkKernelModule = pkgs: kernel: pkgs.stdenv.mkDerivation {
        pname = "jitterentropy-kmod";
        version = "${version}-${kernel.version}";
        inherit src;

        nativeBuildInputs = kernel.moduleBuildDependencies;

        makeFlags = [
          "-f" "Makefile.kernel"
          "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          "modules"
        ];

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib/modules/${kernel.modDirVersion}/extra
          install -m644 kernel/*.ko \
            $out/lib/modules/${kernel.modDirVersion}/extra/
          runHook postInstall
        '';
      };

      # Cross build of the userspace library for Windows with mingw-w64. This
      # exercises the Windows code paths in the arch headers (QPC/RDTSC timer,
      # VirtualAlloc secure memory, GetActiveProcessorCount, ...). Only object
      # compilation + static archiving is done, which is enough to validate the
      # cross compilation.
      mkMingwLib = pkgsCross: pkgsCross.stdenv.mkDerivation {
        pname = "jitterentropy-mingw";
        inherit version src;
        hardeningDisable = [ "fortify" ];

        # winpthreads headers for <pthread.h> used by the timer thread support.
        buildInputs = [ pkgsCross.windows.pthreads ];

        buildPhase = ''
          runHook preBuild
          for f in src/*.c; do
            echo "  CC (mingw) $f"
            $CC -O0 -std=c11 -DJENT_CONF_ENABLE_INTERNAL_TIMER \
                -I. -Isrc -c "$f" -o "$(basename "$f" .c).o"
          done
          $AR rcs libjitterentropy.a ./*.o
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib $out/include
          install -m644 libjitterentropy.a $out/lib/
          install -m644 jitterentropy.h    $out/include/
          runHook postInstall
        '';

        dontStrip = true;
      };
    in
    {
      packages = forAllSystems (system: pkgs: {
        default                   = mkCoreLib pkgs.stdenv;
        jitterentropy             = mkCoreLib pkgs.stdenv;
        jitterentropy-musl        = mkCoreLib pkgs.pkgsMusl.stdenv;
        jitterentropy-cmake       = mkCmakeLib pkgs pkgs.stdenv;

        # External crypto backends (SHA-3 conditioner / secure memory / FIPS).
        jitterentropy-openssl = mkCryptoLib pkgs "OPENSSL" {
          buildInputs = [ pkgs.openssl ];
          includeDir  = "${pkgs.openssl.dev}/include";
          libraryFile = "${pkgs.lib.getLib pkgs.openssl}/lib/libcrypto.so";
        };
        jitterentropy-gcrypt = mkCryptoLib pkgs "LIBGCRYPT" {
          buildInputs = [ pkgs.libgcrypt pkgs.libgpg-error ];
          includeDir  = "${pkgs.libgcrypt.dev}/include";
          libraryFile = "${pkgs.lib.getLib pkgs.libgcrypt}/lib/libgcrypt.so";
        };
        jitterentropy-awslc = mkCryptoLib pkgs "AWSLC" {
          buildInputs = [ pkgs.aws-lc ];
          includeDir  = "${pkgs.lib.getDev pkgs.aws-lc}/include";
          libraryFile = "${pkgs.lib.getLib pkgs.aws-lc}/lib/libcrypto.so";
        };
        jitterentropy-mingw       = mkMingwLib pkgs.pkgsCross.mingwW64;
        jitterentropy-status      = mkStatusTool pkgs pkgs.stdenv;
        jitterentropy-status-musl = mkStatusTool pkgs.pkgsMusl pkgs.pkgsMusl.stdenv;
        kernel-module             = mkKernelModule pkgs pkgs.linuxPackages.kernel;
      });

      # `nix flake check` builds the glibc and musl libraries (with their
      # runtime smoke tests), the mingw-w64 cross build, the userspace status
      # helper, the kernel module and runs the NixOS VM test that loads the
      # module and exercises the devices, the status ioctl and the hwrng.
      checks = forAllSystems (system: pkgs:
        let p = self.packages.${system}; in {
          inherit (p)
            jitterentropy
            jitterentropy-musl
            jitterentropy-cmake
            jitterentropy-openssl
            jitterentropy-gcrypt
            jitterentropy-awslc
            jitterentropy-mingw
            jitterentropy-status
            jitterentropy-status-musl
            kernel-module;

          vm-test = pkgs.testers.nixosTest (import ./nix/vm-test.nix {
            inherit pkgs;
            kernelModule = p.kernel-module;
            statusTool = p.jitterentropy-status;
            cmakeTools = p.jitterentropy-cmake;
          });
        });

      devShells = forAllSystems (system: pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.gnumake
            pkgs.gcc
            pkgs.linuxHeaders
            pkgs.jq
          ];
          inputsFrom = [ self.packages.${system}.jitterentropy ];
        };
      });

      formatter = forAllSystems (system: pkgs: pkgs.nixpkgs-fmt);
    };
}
