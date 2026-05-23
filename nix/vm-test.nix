# NixOS VM test for the jitterentropy kernel module.
#
# Boots a VM that auto-loads jitterentropy_drv.ko, exercises both character
# devices, the status ioctl (via the userspace tool) and the hwrng backend.
# All userspace tools are made available through environment.systemPackages.
{ pkgs, kernelModule, statusTool, cmakeTools }:

{
  name = "jitterentropy-kmod";

  nodes.machine = { lib, ... }: {
    # The module is built against pkgs.linuxPackages.kernel, so the VM must
    # run that very kernel.
    boot.kernelPackages = pkgs.linuxPackages;
    boot.extraModulePackages = [ kernelModule ];
    # Load at boot with a non-default oversampling rate and the AIS 20/31
    # NTG.1 mode enabled (JENT_NTG1 = 1 << 6 = 64) to also exercise the module
    # parameters. NTG.1 implies the internal timer is disabled, which matches
    # this hardware-time-stamp-only module build.
    boot.kernelModules = [ "jitterentropy_drv" ];
    boot.extraModprobeConfig = "options jitterentropy_drv osr=3 flags=64 quality=256";

    # Disable the QEMU/virtio hardware RNG in the guest so jitterentropy is the
    # only registered hwrng backend (the NixOS test VM otherwise exposes
    # virtio-rng via the virtio_rng driver).
    boot.blacklistedKernelModules = [ "virtio_rng" ];

    # All userspace tools: the kernel-device status helper, the CMake-built
    # library and its recording/validation tools, jq to validate the JSON
    # status and xxd to inspect raw entropy output.
    environment.systemPackages = [ statusTool cmakeTools pkgs.jq pkgs.xxd ];
  };

  testScript = ''
    machine.wait_for_unit("multi-user.target")

    with subtest("module is loaded"):
        machine.succeed("lsmod | grep -qw jitterentropy_drv")

    with subtest("module parameters were applied"):
        machine.succeed("test \"$(cat /sys/module/jitterentropy_drv/parameters/osr)\" = 3")
        machine.succeed("test \"$(cat /sys/module/jitterentropy_drv/parameters/flags)\" = 64")

    with subtest("both character devices exist"):
        machine.succeed("test -c /dev/jitterentropy")
        machine.succeed("test -c /dev/jitterentropy-multi")

    with subtest("reading entropy works on both devices"):
        machine.succeed("test \"$(dd if=/dev/jitterentropy bs=32 count=1 status=none | wc -c)\" = 32")
        machine.succeed("test \"$(dd if=/dev/jitterentropy-multi bs=32 count=1 status=none | wc -c)\" = 32")

    with subtest("the status tool returns valid JSON for both devices"):
        machine.succeed("jitterentropy-status /dev/jitterentropy | jq -e .")
        machine.succeed("jitterentropy-status /dev/jitterentropy-multi | jq -e .configuration.osr")
        osr = machine.succeed("jitterentropy-status /dev/jitterentropy | jq -e .configuration.osr").strip()
        assert osr == "3", f"expected osr 3 in status, got {osr}"

    with subtest("NTG.1 mode is active in the status"):
        machine.succeed("jitterentropy-status /dev/jitterentropy | jq -e '.configuration.ntg1Mode == true'")
        machine.succeed("jitterentropy-status /dev/jitterentropy | jq -e '.configuration.flags.JENT_NTG1 == true'")
        # NTG.1 forces the internal timer off.
        machine.succeed("jitterentropy-status /dev/jitterentropy | jq -e '.configuration.internalTimer == false'")

    with subtest("the CMake-built userspace tools are available and work"):
        # jitterentropy-rng <N> writes N blocks of raw entropy to stdout.
        machine.succeed("jitterentropy-rng 1 > /tmp/jent.out 2>/dev/null")
        machine.succeed("test -s /tmp/jent.out")
        machine.succeed("command -v jitterentropy-osr")
        machine.succeed("command -v jitterentropy-hashtime")

    with subtest("hwrng backend is registered and usable"):
        machine.succeed("grep -qw jitterentropy /sys/class/misc/hw_random/rng_available")
        machine.succeed("echo jitterentropy > /sys/class/misc/hw_random/rng_current")
        machine.succeed("test \"$(dd if=/dev/hwrng bs=32 count=1 status=none | wc -c)\" = 32")

    with subtest("module unloads cleanly"):
        # Stop using the hwrng before removing the module.
        machine.succeed("echo none > /sys/class/misc/hw_random/rng_current || true")
        machine.succeed("rmmod jitterentropy_drv")
        machine.fail("test -c /dev/jitterentropy")
  '';
}
