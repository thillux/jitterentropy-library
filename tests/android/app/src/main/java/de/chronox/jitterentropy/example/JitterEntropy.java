/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example;

/**
 * One Jitter RNG entropy collector, with the default oversampling rate and
 * flags.
 *
 * The library takes no locks and a collector belongs to one thread at a time,
 * so the methods are synchronized. Collecting takes milliseconds per 32 bytes:
 * keep it off the UI thread.
 */
public final class JitterEntropy implements AutoCloseable {
    static {
        System.loadLibrary("jitterentropy_jni");
    }

    /** A failed call into the library, carrying the code it returned. */
    public static final class Failure extends Exception {
        Failure(String call, int code) {
            super(call + " failed: " + describe(code) + " (" + code + ")");
        }
    }

    private long handle;

    /**
     * Runs the power-on tests of the noise source, then allocates the
     * collector.
     */
    public JitterEntropy() throws Failure {
        int ret = nativeInit();
        if (ret != 0)
            throw new Failure("jent_entropy_init_ex", ret);

        handle = nativeAlloc();
        if (handle == 0)
            throw new Failure("jent_entropy_collector_alloc", 0);
    }

    /** The library version as "major.minor.patchlevel". */
    public static String version() {
        int v = nativeVersion();
        return (v / 1000000) + "." + (v / 10000 % 100) + "." + (v / 100 % 100);
    }

    /** Returns len bytes from the collector. */
    public synchronized byte[] read(int len) throws Failure {
        byte[] out = new byte[len];
        int ret = nativeRead(checkedHandle(), out);
        if (ret != 0)
            throw new Failure("jent_read_entropy_safe", ret);
        return out;
    }

    /** The JSON status document of this collector. */
    public synchronized String status() {
        return nativeStatus(checkedHandle());
    }

    @Override
    public synchronized void close() {
        nativeFree(handle);
        handle = 0;
    }

    private long checkedHandle() {
        if (handle == 0)
            throw new IllegalStateException("collector closed");
        return handle;
    }

    /** Names a return code of the library. */
    static String describe(int code) {
        switch (code) {
        /* jent_entropy_init_ex() */
        case 1: return "ENOTIME: timer service not available";
        case 2: return "ECOARSETIME: timer too coarse";
        case 3: return "ENOMONOTONIC: timer not monotonic";
        case 6: return "EMINVARVAR: timer variations too small";
        case 8: return "ESTUCK: too many stuck results";
        case 9: return "EHEALTH: health test failed";
        case 10: return "ERCT: RCT failed";
        case 11: return "EHASH: hash self test failed";
        case 12: return "EMEM: cannot allocate memory";
        case 13: return "EGCD: GCD self test failed";
        /* jent_read_entropy() */
        case -1: return "JENT_ERR_EINVAL: invalid collector";
        case -2: return "JENT_ERR_RCT: intermittent RCT failure";
        case -3: return "JENT_ERR_APT: intermittent APT failure";
        case -4: return "JENT_ERR_NOTIME: timer cannot be initialized";
        case -5: return "JENT_ERR_LAG: intermittent lag predictor failure";
        case -6: return "JENT_ERR_RCT_PERMANENT: permanent RCT failure";
        case -7: return "JENT_ERR_APT_PERMANENT: permanent APT failure";
        case -8: return "JENT_ERR_LAG_PERMANENT: permanent lag predictor failure";
        case -9: return "JENT_ERR_RCT_MEM: intermittent RCT with memory failure";
        case -10: return "JENT_ERR_RCT_MEM_PERMANENT: permanent RCT with memory failure";
        case -11: return "JENT_ERR_SELFTEST: self test failed";
        default: return "unknown error";
        }
    }

    private static native int nativeVersion();
    private static native int nativeInit();
    private static native long nativeAlloc();
    private static native void nativeFree(long handle);
    private static native int nativeRead(long handle, byte[] out);
    private static native String nativeStatus(long handle);
}
