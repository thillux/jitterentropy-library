{
  description = ''
    jitterentropy library plus an out-of-tree Linux kernel module
    (jitterentropy_kmod) exposing hwrng + /dev/jitterentropy and a tiny
    UEFI demo application that prints 32 jitterentropy bytes.
  '';

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    # Explicit system list: the Linux kmod / EFI app are Linux-only, but
    # we additionally expose a FreeBSD-only kernel module under
    # x86_64-freebsd / aarch64-freebsd. Each per-system attrset below
    # uses `pkgs.lib.optionalAttrs` to expose only the packages that
    # actually make sense for the host platform.
    flake-utils.lib.eachSystem [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
      "x86_64-freebsd"
      "aarch64-freebsd"
    ] (system:
      let
        pkgs = import nixpkgs { inherit system; };
        inherit (pkgs) lib;
        hostPlat = pkgs.stdenv.hostPlatform;

        # ---------------- user-space shared / static library ----------------
        jitterentropy = pkgs.stdenv.mkDerivation {
          pname = "jitterentropy";
          version = "3.7.1";
          src = ./.;
          enableParallelBuilding = true;
          # The library's own Makefile insists on -O0 (see jitterentropy-base.c).
          dontStrip = true;
          makeFlags = [ "PREFIX=$(out)" ];
          installTargets = [ "install" "install-static" ];
          meta = with pkgs.lib; {
            description = "CPU jitter based entropy source library";
            homepage = "https://www.chronox.de/jent.html";
            license = with licenses; [ bsd3 gpl2Plus ];
            platforms = platforms.unix;
          };
        };

        # ---------------- Linux kernel module ------------------------------
        # Build against the kernel from a chosen linuxPackages set. Override
        # with `nix build .#jitterentropy-kmod --override-input kernel ...`
        # or by importing this flake and substituting `kernel`.
        mkJitterKmod = { kernel }: pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-kmod";
          version = "3.7.1-${kernel.version}";
          src = ./.;

          nativeBuildInputs = kernel.moduleBuildDependencies;
          hardeningDisable = [ "pic" "format" ];

          # The kernel's own Kbuild system drives the build via the Kbuild
          # file at the project root. We don't want the upstream Makefile
          # to run, so override the build phase.
          buildPhase = ''
            runHook preBuild
            make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
                 M=$PWD modules
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm0644 jitterentropy_kmod.ko \
                $out/lib/modules/${kernel.modDirVersion}/extra/jitterentropy_kmod.ko
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description =
              "Linux kernel module exposing jitterentropy as hwrng + /dev/jitterentropy";
            license = licenses.gpl2Plus;
            platforms = platforms.linux;
          };
        };

        jitterentropy-kmod =
          if hostPlat.isLinux
          then mkJitterKmod { kernel = pkgs.linuxPackages.kernel; }
          else null;

        # ---------------- macOS (xnu) KEXT ---------------------------------
        # Plain-C, cdevsw-based KEXT - no IOKit C++ runtime needed.
        # The Kernel.framework headers ship with Xcode (Command Line
        # Tools is insufficient); xcrun locates the right SDK.
        #
        # Important caveats baked into the macos/Makefile:
        #   - Apple deprecated third-party KEXTs in macOS 11.
        #   - Loading requires reduced SIP on Intel and "Reduced
        #     Security" boot mode on Apple Silicon.
        #   - The bundle is ad-hoc signed; production distribution
        #     needs an Apple-issued kext-signing entitlement.
        jitterentropy-macos-kext =
          if hostPlat.isDarwin
          then pkgs.stdenv.mkDerivation {
            pname = "jitterentropy-macos-kext";
            version = "3.7.1";
            src = ./.;

            # Xcode tools (xcrun, clang, kextlibs) are expected to be
            # present on the host. nixpkgs offers an `xcbuild` shim
            # which is enough for non-DriverKit KEXTs.
            nativeBuildInputs = [ pkgs.xcbuild ];

            hardeningDisable = [ "pic" "stackprotector" "fortify" ];

            buildPhase = ''
              runHook preBuild
              make -C macos ARCH=${hostPlat.darwinArch or hostPlat.parsed.cpu.name}
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/Library/Extensions
              cp -R macos/build/jitterentropy_kmod.kext \
                  $out/Library/Extensions/jitterentropy_kmod.kext
              runHook postInstall
            '';

            meta = with lib; {
              description =
                "macOS KEXT exposing jitterentropy as /dev/jitterentropy";
              license = licenses.bsd2;
              platforms = platforms.darwin;
            };
          }
          else null;

        # ---------------- FreeBSD kernel module ----------------------------
        # Notes on why this is *not* built via pkgs.freebsd.mkDerivation:
        # nixpkgs' freebsd attrset has an evaluation recursion on the
        # current unstable channel (bmake -> libc -> include -> rpcgen
        # -> bmake), so any reference to pkgs.freebsd.sys.src or
        # pkgs.freebsd.mkDerivation poisons `nix flake check`. We
        # instead use a plain stdenv + bmake and let bsd.kmod.mk find
        # the FreeBSD kernel sources the conventional way: either via
        # /usr/src/sys on a FreeBSD host or via the user-supplied
        # SYSDIR makeFlag.
        #
        # Use:
        #   nix build .#jitterentropy-freebsd-kmod                 (on FreeBSD)
        #   nix build .#jitterentropy-freebsd-kmod \
        #             --argstr SYSDIR /path/to/freebsd/sys
        jitterentropy-freebsd-kmod =
          if hostPlat.isFreeBSD
          then pkgs.stdenv.mkDerivation {
            pname = "jitterentropy-freebsd-kmod";
            version = "3.7.1";
            src = ./.;

            nativeBuildInputs = [ pkgs.bmake ];

            hardeningDisable = [ "pic" "stackprotector" "fortify" ];

            buildPhase = ''
              runHook preBuild
              # bsd.kmod.mk defaults SYSDIR to /usr/src/sys; the FreeBSD
              # host build environment ships those sources in place.
              # Override via `make SYSDIR=...` at build time when they
              # live elsewhere.
              bmake -C freebsd ''${SYSDIR:+SYSDIR=$SYSDIR}
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              install -Dm0555 freebsd/jitterentropy_kmod.ko \
                  $out/boot/modules/jitterentropy_kmod.ko
              runHook postInstall
            '';

            meta = with lib; {
              description =
                "FreeBSD kernel module exposing jitterentropy as /dev/jitterentropy";
              license = licenses.bsd2;
              platforms = platforms.freebsd;
            };
          }
          else null;

        # ---------------- EFI demo application -----------------------------
        # gnu-efi's nixpkgs derivation is Linux-only; on Darwin / FreeBSD
        # we just omit this package rather than try to chase a portable
        # cross-build.
        jitterentropy-efi =
          if hostPlat.isLinux
          then mkJitterEfi
          else null;

        mkJitterEfi = pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-efi";
          version = "3.7.1";
          src = ./.;

          nativeBuildInputs = [ pkgs.gnu-efi ];

          buildPhase = ''
            runHook preBuild
            make -C efi \
                EFI_INC=${pkgs.gnu-efi}/include/efi \
                EFI_LIB=${pkgs.gnu-efi}/lib \
                EFI_LIBDIR=${pkgs.gnu-efi}/lib \
                EFI_CRT0=${pkgs.gnu-efi}/lib/crt0-efi-${
                  if pkgs.stdenv.hostPlatform.isAarch64 then "aarch64"
                  else if pkgs.stdenv.hostPlatform.isi686 then "ia32"
                  else "x86_64"
                }.o \
                EFI_LDS=${pkgs.gnu-efi}/lib/elf_${
                  if pkgs.stdenv.hostPlatform.isAarch64 then "aarch64"
                  else if pkgs.stdenv.hostPlatform.isi686 then "ia32"
                  else "x86_64"
                }_efi.lds
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm0644 efi/build/jitterentropy.efi \
                $out/share/jitterentropy/jitterentropy.efi
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description =
              "Tiny UEFI app that prints 32 random bytes from jitterentropy";
            license = with licenses; [ bsd3 gpl2Plus ];
            platforms = [ "x86_64-linux" "aarch64-linux" "i686-linux" ];
          };
        };

      in {
        packages = lib.filterAttrs (_: v: v != null) {
          default                    = jitterentropy;
          jitterentropy              = jitterentropy;
          jitterentropy-kmod         = jitterentropy-kmod;
          jitterentropy-efi          = jitterentropy-efi;
          jitterentropy-freebsd-kmod = jitterentropy-freebsd-kmod;
          jitterentropy-macos-kext   = jitterentropy-macos-kext;
        };

        # Re-exported so downstream flakes can build the kmod against a
        # different kernel: `(jitterentropy.lib.${system}.mkKmod { kernel = ...; })`
        lib.mkKmod = mkJitterKmod;

        devShells.default = pkgs.mkShell {
          buildInputs = [ pkgs.gnumake ]
            ++ lib.optionals (!hostPlat.isDarwin) [ pkgs.gcc ]
            ++ lib.optionals hostPlat.isLinux [
                 pkgs.gnu-efi
                 pkgs.linuxPackages.kernel.dev
               ]
            ++ lib.optionals hostPlat.isFreeBSD [
                 pkgs.bmake
                 pkgs.freebsd.sys
               ]
            ++ lib.optionals hostPlat.isDarwin [
                 pkgs.xcbuild
               ];
        };
      });
}
