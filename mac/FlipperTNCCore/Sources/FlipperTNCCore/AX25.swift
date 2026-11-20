import Foundation

public enum AX25Error: Error, Equatable {
    case badCallsign(String)
    case tooManyDigis
    case tooLong(Int)
    case malformed(String)
}

public struct AX25Address: Equatable, CustomStringConvertible {
    public var call: String
    public var ssid: UInt8
    /// Has-been-repeated bit on digipeaters (C bit on source/destination).
    public var hBit: Bool

    public init(call: String, ssid: UInt8 = 0, hBit: Bool = false) {
        self.call = call
        self.ssid = ssid
        self.hBit = hBit
    }

    /// Parses "CALL" or "CALL-SSID" (optionally with a trailing "*").
    public init(parsing text: String) throws {
        var t = text.trimmingCharacters(in: .whitespaces).uppercased()
        var star = false
        if t.hasSuffix("*") { star = true; t.removeLast() }
        let parts = t.split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count <= 2, let callPart = parts.first, (1...6).contains(callPart.count),
            callPart.allSatisfy({ ($0 >= "A" && $0 <= "Z") || ($0 >= "0" && $0 <= "9") })
        else { throw AX25Error.badCallsign(text) }
        var ssid: UInt8 = 0
        if parts.count == 2 {
            guard (1...2).contains(parts[1].count), let v = UInt8(parts[1]), v <= 15 else {
                throw AX25Error.badCallsign(text)
            }
            ssid = v
        }
        self.init(call: String(callPart), ssid: ssid, hBit: star)
    }

    public func encode(last: Bool) -> [UInt8] {
        let padded = call.padding(toLength: 6, withPad: " ", startingAt: 0)
        var out = padded.utf8.map { $0 << 1 }
        out.append((hBit ? 0x80 : 0) | 0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0))
        return out
    }

    public static func decode(_ raw: ArraySlice<UInt8>) -> (AX25Address, Bool) {
        let b = Array(raw)
        let call = String(decoding: b[0..<6].map { $0 >> 1 }, as: UTF8.self)
            .trimmingCharacters(in: .whitespaces)
        return (AX25Address(call: call, ssid: (b[6] >> 1) & 0x0F, hBit: b[6] & 0x80 != 0), b[6] & 1 != 0)
    }

    public var description: String { ssid == 0 ? call : "\(call)-\(ssid)" }
}

public struct AX25Frame: Equatable, CustomStringConvertible {
    public static let maxLength = 330
    public static let controlUI: UInt8 = 0x03
    public static let pidNone: UInt8 = 0xF0

    public var source: AX25Address
    public var destination: AX25Address
    public var path: [AX25Address]
    public var control: UInt8
    public var pid: UInt8?
    public var info: [UInt8]

    public init(source: AX25Address, destination: AX25Address, path: [AX25Address] = [],
                info: [UInt8], control: UInt8 = AX25Frame.controlUI, pid: UInt8? = AX25Frame.pidNone) {
        self.source = source
        self.destination = destination
        self.path = path
        self.info = info
        self.control = control
        self.pid = pid
    }

    /// UI frame from strings. `path` is comma-separated ("WIDE1-1,WIDE2-1").
    public static func ui(source: String, destination: String, path: String = "", text: String) throws -> AX25Frame {
        let digis = try path.split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
            .map { try AX25Address(parsing: $0) }
        return AX25Frame(source: try AX25Address(parsing: source),
                         destination: try AX25Address(parsing: destination),
                         path: digis, info: Array(text.utf8))
    }

    public func encode() throws -> [UInt8] {
        guard path.count <= 8 else { throw AX25Error.tooManyDigis }
        var dst = destination; dst.hBit = true   // AX.25 v2 command frame
        var src = source; src.hBit = false
        let addrs = [dst, src] + path
        var out: [UInt8] = []
        for (i, a) in addrs.enumerated() { out += a.encode(last: i == addrs.count - 1) }
        out.append(control)
        if let pid { out.append(pid) }
        out += info
        guard out.count <= AX25Frame.maxLength else { throw AX25Error.tooLong(out.count) }
        return out
    }

    public static func decode(_ raw: [UInt8]) throws -> AX25Frame {
        var addrs: [AX25Address] = []
        var i = 0
        while true {
            guard i + 7 <= raw.count else { throw AX25Error.malformed("truncated address field") }
            let (a, last) = AX25Address.decode(raw[i..<i + 7])
            addrs.append(a)
            i += 7
            if last { break }
            guard addrs.count < 10 else { throw AX25Error.malformed("address field too long") }
        }
        guard addrs.count >= 2 else { throw AX25Error.malformed("need source and destination") }
        guard i < raw.count else { throw AX25Error.malformed("missing control") }
        let control = raw[i]; i += 1
        var pid: UInt8?
        if (control & 0x01) == 0 || (control & 0xEF) == controlUI, i < raw.count {
            pid = raw[i]; i += 1
        }
        return AX25Frame(source: addrs[1], destination: addrs[0], path: Array(addrs.dropFirst(2)),
                         info: Array(raw[i...]), control: control, pid: pid)
    }

    public var route: String {
        let via = path.map { $0.description + ($0.hBit ? "*" : "") }
        return via.isEmpty ? "\(source) → \(destination)" : "\(source) → \(destination) via \(via.joined(separator: ","))"
    }

    public var infoText: String {
        String(info.map { (0x20..<0x7F).contains($0) ? Character(UnicodeScalar($0)) : "." })
    }

    public var description: String {
        let via = path.map { "," + $0.description + ($0.hBit ? "*" : "") }.joined()
        return "\(source)>\(destination)\(via): \(infoText)"
    }
}

/// CRC-16/X.25, the HDLC FCS. Only needed for debugging; the device adds/checks it.
public func ax25FCS(_ bytes: [UInt8]) -> UInt16 {
    var crc: UInt16 = 0xFFFF
    for b in bytes {
        crc ^= UInt16(b)
        for _ in 0..<8 { crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x8408 : crc >> 1 }
    }
    return crc ^ 0xFFFF
}

public extension Array where Element == UInt8 {
    var hexString: String { map { String(format: "%02x", $0) }.joined() }

    init?(hex: String) {
        let clean = hex.filter { !$0.isWhitespace }
        guard clean.count % 2 == 0 else { return nil }
        var out: [UInt8] = []
        var idx = clean.startIndex
        while idx < clean.endIndex {
            let next = clean.index(idx, offsetBy: 2)
            guard let b = UInt8(clean[idx..<next], radix: 16) else { return nil }
            out.append(b)
            idx = next
        }
        self = out
    }
}
