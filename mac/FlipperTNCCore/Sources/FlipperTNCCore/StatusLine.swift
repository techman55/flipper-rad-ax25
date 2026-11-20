import Foundation

/// A SetHardware (0x06) ASCII line from the device, e.g.
/// `RX RSSI=-87 FREQ=433920000 MODE=0 FCS=OK T=12345` or `ERR TX_BLOCKED_REGION SEQ=3`.
public struct StatusLine: Equatable {
    public let kind: String
    public let fields: [String: String]
    public let words: [String]
    public let raw: String

    public init(_ text: String) {
        raw = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let tokens = raw.split(separator: " ").map(String.init)
        kind = tokens.first?.uppercased() ?? ""
        var f: [String: String] = [:]
        var w: [String] = []
        for t in tokens.dropFirst() {
            if let eq = t.firstIndex(of: "=") {
                f[String(t[..<eq]).uppercased()] = String(t[t.index(after: eq)...])
            } else {
                w.append(t)
            }
        }
        fields = f
        words = w
    }

    public init(bytes: [UInt8]) {
        self.init(String(decoding: bytes, as: UTF8.self))
    }

    public func int(_ key: String) -> Int? { fields[key].flatMap { Int($0) } }

    public var errorCode: String? { kind == "ERR" ? words.first : nil }

    /// OK, or ERR without a SEQ: an answer to the last SetHardware request.
    public var isReply: Bool { kind == "OK" || (kind == "ERR" && fields["SEQ"] == nil) }
}
