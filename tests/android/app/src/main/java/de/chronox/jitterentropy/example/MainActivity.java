/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Allocates one collector at start-up and offers its status and 32 bytes of
 * its output, each on a button press.
 */
public final class MainActivity extends Activity {
    private static final String TAG = "JitterEntropyExample";

    /*
     * One thread for every call into the library: the power-on tests and
     * collection take long enough to freeze the UI, and a collector belongs
     * to one thread at a time.
     */
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private JitterEntropy collector;

    private TextView state;
    private TextView output;
    private Button statusButton;
    private Button generateButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        state = findViewById(R.id.state);
        output = findViewById(R.id.output);
        statusButton = findViewById(R.id.status_button);
        generateButton = findViewById(R.id.generate_button);

        ((TextView)findViewById(R.id.version))
            .setText(getString(R.string.version, JitterEntropy.version()));

        statusButton.setOnClickListener(v -> worker.execute(this::showStatus));
        generateButton.setOnClickListener(v -> worker.execute(this::generate));

        worker.execute(this::allocate);
    }

    @Override
    protected void onDestroy() {
        worker.execute(() -> {
            if (collector != null)
                collector.close();
        });
        worker.shutdown();
        super.onDestroy();
    }

    private void allocate() {
        try {
            collector = new JitterEntropy();
            Log.i(TAG, "collector allocated");
            runOnUiThread(() -> {
                state.setText(R.string.state_ready);
                statusButton.setEnabled(true);
                generateButton.setEnabled(true);
            });
        } catch (JitterEntropy.Failure e) {
            Log.e(TAG, e.getMessage());
            runOnUiThread(() -> state.setText(e.getMessage()));
        }
    }

    private void showStatus() {
        String status = collector.status();
        Log.i(TAG, "status: " + status);
        runOnUiThread(() -> output.setText(
            status != null ? status : getString(R.string.status_failed)));
    }

    private void generate() {
        try {
            String hex = toHex(collector.read(32));
            Log.i(TAG, "32 bytes: " + hex);
            runOnUiThread(() -> output.setText(hex));
        } catch (JitterEntropy.Failure e) {
            Log.e(TAG, e.getMessage());
            runOnUiThread(() -> output.setText(e.getMessage()));
        }
    }

    private static String toHex(byte[] data) {
        StringBuilder sb = new StringBuilder(data.length * 2);
        for (byte b : data)
            sb.append(String.format("%02x", b & 0xff));
        return sb.toString();
    }
}
