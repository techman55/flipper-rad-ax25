import Foundation

/// KISS framing (FEND/FESC escaping) and a streaming decoder.
public enum KISS {
    public static let fend: UInt8 = 0xC0
    public static let fesc: UInt8 = 0xDB
    public static let tfend: UInt8 = 0xDC
    public static let tfesc: UInt8 = 0xDD

    public static let cmdData: UInt8 = 0x00
    public static let cmdTxDelay: UInt8 = 0x01
    public static let cmdP: UInt8 = 0x02
    public static let cmdSlotTime: UInt8 = 0x03
    public static let cmdTxTail: UInt8 = 0x04
    public static let cmdSetHardware: UInt8 = 0x06
    public static let cmdAckMode: UInt8 = 0x0C

    public static func escape(_ bytes: [UInt8]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(bytes.count + 8)
        for b in bytes {
            switch b {
            case fend: out += [fesc, tfend]
            case fesc: out += [fesc, tfesc]
            default: out.append(b)
            }
        }
        return out
    }

    public static func encode(type: UInt8, payload: [UInt8] = []) -> [UInt8] {
        [fend] + escape([type] + payload) + [fend]
    }

    public static func data(_ ax25: [UInt8]) -> [UInt8] {
        encode(type: cmdData, payload: ax25)
    }

    public static func ackMode(seq: UInt16, _ ax25: [UInt8]) -> [UInt8] {
        encode(type: cmdAckMode, payload: [UInt8(seq >> 8), UInt8(seq & 0xFF)] + ax25)
    }

    public static func setHardware(_ line: String) -> [UInt8] {
        encode(type: cmdSetHardware, payload: Array(line.utf8))
    }
}

public struct KISSFrame: Equatable {
    public let type: UInt8
    public let payload: [UInt8]
    public var port: UInt8 { type >> 4 }
    public var command: UInt8 { type & 0x0F }
}

/// Feed arbitrary chunks of bytes, get whole frames back.
public struct KISSDecoder {
    private var buf: [UInt8] = []
    private var inFrame = false
    private var escape = false
    private let maxLen: Int

    public init(maxLen: Int = 4096) { self.maxLen = maxLen }

    public mutating func feed<S: Sequence>(_ bytes: S) -> [KISSFrame] where S.Element == UInt8 {
        var frames: [KISSFrame] = []
        for b in bytes {
            if b == KISS.fend {
                if inFrame, let t = buf.first {
                    frames.append(KISSFrame(type: t, payload: Array(buf.dropFirst())))
                }
                buf.removeAll(keepingCapacity: true)
                inFrame = true
                escape = false
                continue
            }
            guard inFrame else { continue }
            var v = b
            if escape {
                escape = false
                if b == KISS.tfend { v = KISS.fend } else if b == KISS.tfesc { v = KISS.fesc }
            } else if b == KISS.fesc {
                escape = true
                continue
            }
            if buf.count < maxLen {
                buf.append(v)
            } else {
                buf.removeAll()
                inFrame = false  // runaway frame: resync on next FEND
            }
        }
        return frames
    }
}
