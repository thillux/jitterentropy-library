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
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val TAG = "JitterEntropyExample"

/**
 * Owns the collector, so that it outlives the activity being recreated - a
 * rotation would otherwise rerun the power-on tests.
 */
class CollectorViewModel : ViewModel() {
    var state by mutableStateOf("Allocating the entropy collector…")
        private set
    var output by mutableStateOf("")
        private set
    var ready by mutableStateOf(false)
        private set

    /*
     * One thread for every call into the library: the power-on tests and
     * collection take long enough to freeze the UI, and a collector belongs
     * to one thread at a time.
     */
    private val worker = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
    private var collector: JitterEntropy? = null

    init {
        viewModelScope.launch {
            try {
                collector = withContext(worker) { JitterEntropy() }
                Log.i(TAG, "collector allocated")
                state = "Entropy collector allocated"
                ready = true
            } catch (e: JitterEntropy.Failure) {
                Log.e(TAG, e.message!!)
                state = e.message!!
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
        // Queued behind whatever the worker is still running.
        val c = collector
        worker.executor.execute { c?.close() }
        worker.close()
    }
}
