/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import Combine
import Foundation

/// One Jitter RNG entropy collector, with the default oversampling rate and
/// no flags.
///
/// Every call into the library runs on `queue`: the power-on tests and
/// collection take long enough to freeze the UI, and a collector must not be
/// used by two threads at once - which a serial queue rules out.
final class Collector: ObservableObject {
    @Published private(set) var state = "Allocating the entropy collector…"
    @Published private(set) var output = ""
    @Published private(set) var ready = false

    /* The collector must be allocated with what the power-on tests ran with. */
    private static let osr: UInt32 = 0
    private static let flags: UInt32 = 0

    private let queue = DispatchQueue(label: "de.chronox.jitterentropy.example")
    /* struct rand_data *, replaced in place by jent_read_entropy_safe(). */
    private var ec: OpaquePointer?

    static var version: String {
        let v = jent_version()
        return "\(v / 1_000_000).\(v / 10_000 % 100).\(v / 100 % 100)"
    }

    init() {
        queue.async { self.allocate() }
    }

    deinit {
        let ec = self.ec
        queue.async { jent_entropy_collector_free(ec) }
    }

    private func allocate() {
        let text: String
        let ret = jent_entropy_init_ex(Self.osr, Self.flags)

        if ret != 0 {
            text = "jent_entropy_init_ex failed: \(Self.describe(ret)) (\(ret))"
        } else {
            ec = jent_entropy_collector_alloc(Self.osr, Self.flags)
            text = ec != nil ? "Entropy collector allocated"
                             : "jent_entropy_collector_alloc failed"
        }

        NSLog("%@", text)
        let ready = ec != nil
        DispatchQueue.main.async {
            self.state = text
            self.ready = ready
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
            buf.withUnsafeMutableBytes { memset_s($0.baseAddress, $0.count, 0, $0.count) }

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
