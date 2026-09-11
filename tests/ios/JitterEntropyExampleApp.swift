/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import SwiftUI

/// Allocates one collector at start-up and offers its status and 32 bytes of
/// its output, each on a button press.
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
