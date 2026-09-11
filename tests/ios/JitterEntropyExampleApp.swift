/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import SwiftUI

/// Allocates one collector at start-up and offers its status and 32 bytes of
/// its output, each on a button press. Three more replace it with a new
/// collector: without flags, in FIPS or in NTG.1 mode, on the platform clock
/// or on the library's timer thread as the toggle above them says.
@main
struct JitterEntropyExampleApp: App {
    @StateObject private var collector = Collector()

    var body: some Scene {
        WindowGroup {
            ContentView(collector: collector)
        }
    }
}

struct ContentView: View {
    @ObservedObject var collector: Collector

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Jitter RNG \(Collector.version)")
                .font(.title2)
            Text(collector.state)

            Text("New collector")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Toggle("Timer thread", isOn: $collector.timerThread)
                .disabled(collector.allocating)
            HStack(spacing: 12) {
                Button("Default") { collector.allocate(.standard) }
                    .frame(maxWidth: .infinity)
                Button("FIPS") { collector.allocate(.fips) }
                    .frame(maxWidth: .infinity)
                /* NTG.1 forbids the timer thread; the library refuses it. */
                Button("NTG.1") { collector.allocate(.ntg1) }
                    .frame(maxWidth: .infinity)
                    .disabled(collector.timerThread)
            }
            .buttonStyle(.bordered)
            .disabled(collector.allocating)
            if collector.timerThread || collector.timerThreadUsed {
                Text("Once a collector passed its power-on tests on the timer thread, every later one uses it too, and NTG.1 is refused until the app restarts.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 12) {
                Button("Show status") { collector.showStatus() }
                    .frame(maxWidth: .infinity)
                Button("32 bytes") { collector.generate() }
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!collector.ready)

            ScrollView {
                Text(collector.output)
                    .font(.system(.footnote, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
    }
}
