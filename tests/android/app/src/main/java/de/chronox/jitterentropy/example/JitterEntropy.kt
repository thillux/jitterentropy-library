/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

/**
 * One Jitter RNG entropy collector, with the default oversampling rate and
 * no flags. The constructor runs the power-on tests of the noise source, then
 * allocates the collector.
 *
 * The library takes no locks and a collector belongs to one thread at a time,
 * so the methods are synchronized. Collecting takes milliseconds per 32 bytes:
 * keep it off the main thread.
 */
class JitterEntropy : AutoCloseable {
    /** A failed call into the library, carrying the code it returned. */
    class Failure(call: String, code: Int) :
        Exception("$call failed: ${describe(code)} ($code)")

    private var handle = 0L

    init {
        val ret = nativeInit()
        if (ret != 0)
            throw Failure("jent_entropy_init_ex", ret)

        handle = nativeAlloc()
        if (handle == 0L)
            throw Failure("jent_entropy_collector_alloc", 0)
    }

    /** Returns [len] bytes from the collector. */
    @Synchronized
    fun read(len: Int): ByteArray {
        val out = ByteArray(len)
        val ret = nativeRead(checkedHandle(), out)
        if (ret != 0)
            throw Failure("jent_read_entropy_safe", ret)
        return out
    }

    /** The JSON status document of this collector. */
    @Synchronized
    fun status(): String? = nativeStatus(checkedHandle())

    @Synchronized
    override fun close() {
        nativeFree(handle)
        handle = 0L
    }

    private fun checkedHandle(): Long {
        check(handle != 0L) { "collector closed" }
        return handle
    }

    companion object {
        init {
            System.loadLibrary("jitterentropy_jni")
        }

        /** The library version as "major.minor.patchlevel". */
        val version: String
            get() {
                val v = nativeVersion()
                return "${v / 1000000}.${v / 10000 % 100}.${v / 100 % 100}"
            }

        /** Names a return code of the library. */
        fun describe(code: Int): String = when (code) {
            /* jent_entropy_init_ex() */
            1 -> "ENOTIME: timer service not available"
            2 -> "ECOARSETIME: timer too coarse"
            3 -> "ENOMONOTONIC: timer not monotonic"
            6 -> "EMINVARVAR: timer variations too small"
            8 -> "ESTUCK: too many stuck results"
            9 -> "EHEALTH: health test failed"
            10 -> "ERCT: RCT failed"
            11 -> "EHASH: hash self test failed"
            12 -> "EMEM: cannot allocate memory"
            13 -> "EGCD: GCD self test failed"
            /* jent_read_entropy() */
            -1 -> "JENT_ERR_EINVAL: invalid collector"
            -2 -> "JENT_ERR_RCT: intermittent RCT failure"
            -3 -> "JENT_ERR_APT: intermittent APT failure"
            -4 -> "JENT_ERR_NOTIME: timer cannot be initialized"
            -5 -> "JENT_ERR_LAG: intermittent lag predictor failure"
            -6 -> "JENT_ERR_RCT_PERMANENT: permanent RCT failure"
            -7 -> "JENT_ERR_APT_PERMANENT: permanent APT failure"
            -8 -> "JENT_ERR_LAG_PERMANENT: permanent lag predictor failure"
            -9 -> "JENT_ERR_RCT_MEM: intermittent RCT with memory failure"
            -10 -> "JENT_ERR_RCT_MEM_PERMANENT: permanent RCT with memory failure"
            -11 -> "JENT_ERR_SELFTEST: self test failed"
            else -> "unknown error"
        }

        /*
         * @JvmStatic puts these into JitterEntropy itself as static native
         * methods, which is the class the JNI function names in
         * jitterentropy-jni.c refer to.
         */
        @JvmStatic private external fun nativeVersion(): Int
        @JvmStatic private external fun nativeInit(): Int
        @JvmStatic private external fun nativeAlloc(): Long
        @JvmStatic private external fun nativeFree(handle: Long)
        @JvmStatic private external fun nativeRead(handle: Long, out: ByteArray): Int
        @JvmStatic private external fun nativeStatus(handle: Long): String?
    }
}
