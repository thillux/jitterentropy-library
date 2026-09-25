# Tests of Entropy during early boot

This test collects the first 1,001 entropy event values generated during boot
of the Linux kernel. The collection of raw entropy after reboot is compliant
to SP800-90B section 3.1.4.

The test infrastructure sets the Linux system up to reboot the system
some 1,000 times to collect these 1,001 event values.

# Test procedure

See boottime_test_record.sh. The test applies to the Jitter RNG of the vanilla
kernel with `CONFIG_CRYPTO_JITTERENTROPY_TESTINTERFACE`, booted with
`jitterentropy_testing.boot_raw_hires_test=1`.

The result is one file per boot operation,
`/root/results-measurements/jent-raw-noise-restart.<run>.data`, holding the
1,001 successive time deltas recorded by the Jitter RNG, one per line.

The number of files equals the number of reboots.

# Test analysis

Copy the obtained files into `results-measurements` and process them by
invoking `validation-restart/processdata.sh`.
