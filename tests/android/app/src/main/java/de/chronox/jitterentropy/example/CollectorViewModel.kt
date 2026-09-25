/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import java.util.concurrent.Executors
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.launch
import kotlinx.coroutines.plus
import kotlinx.coroutines.withContext

private const val TAG = "JitterEntropyExample"

/**
 * Owns the collector, so that it outlives the activity being recreated - a
 * rotation would otherwise rerun the power-on tests.
 */
class CollectorViewModel : ViewModel() {
    var state by mutableStateOf("")
        private set
    var output by mutableStateOf("")
        private set
    var ready by mutableStateOf(false)
        private set
    var allocating by mutableStateOf(false)
        private set

    /** Whether the next collector uses the timer thread; see JitterEntropy.Config. */
    var timerThread by mutableStateOf(false)

    /** The initial oversampling rate, memory size and hash loop count of the next collector. */
    var osr by mutableStateOf(JitterEntropy.MIN_OSR)
    var memSize by mutableStateOf(0)
    var hashLoop by mutableStateOf(0)

    /*
     * One thread for every call into the library: the power-on tests and
     * collection take long enough to freeze the UI, and a collector belongs
     * to one thread at a time.
     */
    private val worker = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
    private var collector: JitterEntropy? = null
    private var cleared = false

    /**
     * Takes over [built], on the worker thread that built it - unless the
     * model was cleared in the meantime, in which case nobody is left to
     * close it and it is closed here.
     */
    @Synchronized
    private fun adopt(built: JitterEntropy) {
        if (cleared) built.close() else collector = built
    }

    /** Hands the current collector over and leaves none behind. */
    @Synchronized
    private fun detach(clearing: Boolean = false): JitterEntropy? {
        if (clearing) cleared = true
        val c = collector
        collector = null
        return c
    }

    init {
        allocate(JitterEntropy.Mode.DEFAULT)
    }

    /** Replaces the collector with a new one in [mode] and the settings above. */
    fun allocate(mode: JitterEntropy.Mode) {
        val config = JitterEntropy.Config(mode, timerThread, osr, memSize, hashLoop)

        // Not through the coroutine, which a cleared model cancels before it
        // ran; queued ahead of the allocation below all the same.
        val old = detach()
        worker.executor.execute { old?.close() }

        state = "Allocating the entropy collector ($config)…"
        output = ""
        ready = false
        allocating = true
        viewModelScope.launch {
            try {
                /*
                 * The collector is taken over on the worker thread that built
                 * it, and under NonCancellable.
                 *
                 * withContext() throws CancellationException on resume when
                 * the scope was cancelled while its block ran, and the value
                 * the block produced is discarded with it - so a plain
                 * "collector = withContext(worker) { … }" loses the collector
                 * whenever the model is cleared during an allocation. And
                 * ViewModel.clear() cancels viewModelScope before calling
                 * onCleared(), so onCleared() would find no collector, close
                 * nothing, and then shut the worker down: the struct
                 * rand_data, its memory region and, in a compliance mode, its
                 * locked pages are leaked, JitterEntropy having no finalizer.
                 * Backing out of the activity while osr 20 / 512 MB / hash
                 * loop 128 is being built is all it takes.
                 *
                 * The close path above already takes the same care.
                 */
                withContext(worker + NonCancellable) {
                    adopt(JitterEntropy(config))
                }
                Log.i(TAG, "collector allocated ($config)")
                state = "Entropy collector allocated ($config)"
                ready = true
            } catch (e: JitterEntropy.Failure) {
                Log.e(TAG, e.message!!)
                state = e.message!!
            } finally {
                allocating = false
            }
        }
    }

    fun showStatus() {
        val c = collector ?: return
        viewModelScope.launch {
            val status = withContext(worker) { c.status() }
            Log.i(TAG, "status: $status")
            output = status ?: "jent_status failed"
        }
    }

    fun generate() {
        val c = collector ?: return
        viewModelScope.launch {
            output = try {
                val hex = withContext(worker) {
                    c.read(32).joinToString("") { "%02x".format(it.toInt() and 0xff) }
                }
                Log.i(TAG, "32 bytes: $hex")
                hex
            } catch (e: JitterEntropy.Failure) {
                Log.e(TAG, e.message!!)
                e.message!!
            }
        }
    }

    override fun onCleared() {
        /*
         * Queued behind whatever the worker is still running - including an
         * allocation whose scope clear() has already cancelled: it runs to the
         * end under NonCancellable and either hands its collector over here or,
         * seeing that this ran first, closes it itself. close() shuts the
         * executor down but lets what is already queued run.
         */
        val c = detach(clearing = true)
        worker.executor.execute { c?.close() }
        worker.close()
    }
}
