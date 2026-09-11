/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import Combine
import Foundation

/// One Jitter RNG entropy collector with the default oversampling rate,
/// replaceable by one in another mode or on the timer thread.
///
/// Every call into the library runs on the serial `queue`: it keeps slow
/// calls off the UI, and a collector belongs to one thread at a time.
final class Collector: ObservableObject {
    /// The mode flags. Both compliance modes imply JENT_FORCE_SECURE_MEM, so
    /// they fail with EMEM where the state cannot be locked.
    enum Mode: CustomStringConvertible {
        case standard, fips, ntg1

        var flags: UInt32 {
            switch self {
            case .standard: return 0
            case .fips: return UInt32(JENT_FORCE_FIPS)
            case .ntg1: return UInt32(JENT_NTG1)
            }
        }

        var description: String {
            switch self {
            case .standard: return "default"
            case .fips: return "FIPS"
            case .ntg1: return "NTG.1"
            }
        }
    }

    /// A mode, and whether time stamps come from the library's timer thread
    /// rather than the platform clock (JENT_FORCE_INTERNAL_TIMER).
    ///
    /// The switch is process-wide: once a collector passed its power-on tests
    /// on the timer thread, every later one uses it, and NTG.1, which forbids
    /// it, is refused until the process restarts.
    struct Config: CustomStringConvertible {
        let mode: Mode
        let timerThread: Bool

        var flags: UInt32 {
            mode.flags | (timerThread ? UInt32(JENT_FORCE_INTERNAL_TIMER) : 0)
        }

        var description: String {
            timerThread ? "\(mode), timer thread" : "\(mode)"
        }
    }

    @Published private(set) var state = ""
    @Published private(set) var output = ""
    @Published private(set) var ready = false
    @Published private(set) var allocating = false

    /// Whether the next collector uses the timer thread.
    @Published var timerThread = false
    /// Whether a collector was allocated on the timer thread, which switches
    /// the library to it for the rest of the process.
    @Published private(set) var timerThreadUsed = false

    private static let osr: UInt32 = 0

    private let queue = DispatchQueue(label: "de.chronox.jitterentropy.example")
    /* struct rand_data *, replaced in place by jent_read_entropy_safe(). */
    private var ec: OpaquePointer?

    static var version: String {
        let v = jent_version()
        return "\(v / 1_000_000).\(v / 10_000 % 100).\(v / 100 % 100)"
    }

    init() {
        allocate(.standard)
    }

    deinit {
        let ec = self.ec
        queue.async { jent_entropy_collector_free(ec) }
    }

    /// Replaces the collector with one in `mode`, on the timer thread if
    /// `timerThread` is set. Called on the main queue.
    func allocate(_ mode: Mode) {
        let config = Config(mode: mode, timerThread: timerThread)

        state = "Allocating the entropy collector (\(config))…"
        output = ""
        ready = false
        allocating = true
        queue.async { self.replace(config) }
    }

    private func replace(_ config: Config) {
        jent_entropy_collector_free(ec)
        ec = nil

        /* The collector must be allocated with what the power-on tests ran with. */
        let text: String
        let ret = jent_entropy_init_ex(Self.osr, config.flags)

        if ret != 0 {
            text = "jent_entropy_init_ex failed (\(config)): \(Self.describe(ret)) (\(ret))"
        } else {
            ec = jent_entropy_collector_alloc(Self.osr, config.flags)
            text = ec != nil ? "Entropy collector allocated (\(config))"
                             : "jent_entropy_collector_alloc failed (\(config))"
        }

        NSLog("%@", text)
        let ready = ec != nil
        DispatchQueue.main.async {
            self.state = text
            self.ready = ready
            self.allocating = false
            if ready && config.timerThread {
                self.timerThreadUsed = true
            }
        }
    }

    func showStatus() {
        queue.async {
            /* The size the library documents as holding the whole document. */
            var buf = [CChar](repeating: 0, count: 4096)
            let text = jent_status(self.ec, &buf, buf.count) == 0
                ? String(cString: buf) : "jent_status failed"

            NSLog("status: %@", text)
            self.show(text)
        }
    }

    func generate() {
        queue.async {
            var buf = [CChar](repeating: 0, count: 32)
            let ret = jent_read_entropy_safe(&self.ec, &buf, buf.count)
            let text: String

            if ret < 0 {
                text = "jent_read_entropy_safe failed: \(Self.describe(Int32(ret))) (\(ret))"
            } else {
                text = buf.map { String(format: "%02x", UInt8(bitPattern: $0)) }
                          .joined()
            }

            /* Do not leave the output behind in the buffer. */
            _ = buf.withUnsafeMutableBytes { memset_s($0.baseAddress, $0.count, 0, $0.count) }

            NSLog("32 bytes: %@", text)
            self.show(text)
        }
    }

    private func show(_ text: String) {
        DispatchQueue.main.async { self.output = text }
    }

    /// Names a return code of the library.
    private static func describe(_ code: Int32) -> String {
        switch code {
        /* jent_entropy_init_ex() */
        case 1: return "ENOTIME: timer service not available"
        case 2: return "ECOARSETIME: timer too coarse"
        case 3: return "ENOMONOTONIC: timer not monotonic"
        case 6: return "EMINVARVAR: timer variations too small"
        case 8: return "ESTUCK: too many stuck results"
        case 9: return "EHEALTH: health test failed"
        case 10: return "ERCT: RCT failed"
        case 11: return "EHASH: hash self test failed"
        case 12: return "EMEM: cannot allocate memory"
        case 13: return "EGCD: GCD self test failed"
        /* jent_read_entropy() */
        case -1: return "JENT_ERR_EINVAL: invalid collector"
        case -2: return "JENT_ERR_RCT: intermittent RCT failure"
        case -3: return "JENT_ERR_APT: intermittent APT failure"
        case -4: return "JENT_ERR_NOTIME: timer cannot be initialized"
        case -5: return "JENT_ERR_LAG: intermittent lag predictor failure"
        case -6: return "JENT_ERR_RCT_PERMANENT: permanent RCT failure"
        case -7: return "JENT_ERR_APT_PERMANENT: permanent APT failure"
        case -8: return "JENT_ERR_LAG_PERMANENT: permanent lag predictor failure"
        case -9: return "JENT_ERR_RCT_MEM: intermittent RCT with memory failure"
        case -10: return "JENT_ERR_RCT_MEM_PERMANENT: permanent RCT with memory failure"
        case -11: return "JENT_ERR_SELFTEST: self test failed"
        default: return "unknown error"
        }
    }
}
