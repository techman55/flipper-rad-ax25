import Foundation

/// A received data frame, plus the "RX ..." status line that preceded it.
public struct ReceivedFrame {
    public let raw: [UInt8]
    public let status: StatusLine?
    public let receivedAt: Date

    public var rssi: Int? { status?.int("RSSI") }
    public var fcsOK: Bool { status?.fields["FCS"].map { $0 == "OK" } ?? true }
    public var decoded: AX25Frame? { try? AX25Frame.decode(raw) }
}

public enum TNCEvent {
    case frame(ReceivedFrame)
    case status(StatusLine)
    case ack(UInt16)
    case kiss(KISSFrame)  // anything else, e.g. for a TCP bridge
    case disconnected(String)
}

/// Owns the serial port and a reader thread. `onEvent` and `onRawFrame` are
/// called on the reader thread; hop to the main actor yourself.
public final class TNCConnection {
    public let path: String
    public var onEvent: ((TNCEvent) -> Void)?
    /// Every decoded KISS frame as received (for bridging to TCP clients).
    public var onRawFrame: ((KISSFrame) -> Void)?

    private let port: SerialPort
    private var thread: Thread?
    private let stopFlag = AtomicFlag()
    private var decoder = KISSDecoder()
    private var pendingRX: StatusLine?
    private let writeQueue = DispatchQueue(label: "FlipperTNC.serial.write")

    public init(path: String) throws {
        self.path = path
        port = try SerialPort(path: path)
    }

    public func start() {
        let t = Thread { [weak self] in self?.readLoop() }
        t.name = "FlipperTNC.serial.read"
        t.qualityOfService = .userInitiated
        thread = t
        t.start()
    }

    public func close() {
        stopFlag.set()
        port.close()
    }

    private func readLoop() {
        while !stopFlag.isSet {
            do {
                let bytes = try port.read(timeoutMs: 200)
                if bytes.isEmpty { continue }
                for f in decoder.feed(bytes) { handle(f) }
            } catch {
                if !stopFlag.isSet { onEvent?(.disconnected("\(error)")) }
                stopFlag.set()
            }
        }
    }

    private func handle(_ f: KISSFrame) {
        onRawFrame?(f)
        guard f.port == 0 else { return }
        switch f.command {
        case KISS.cmdSetHardware:
            let st = StatusLine(bytes: f.payload)
            if st.kind == "RX" { pendingRX = st }
            onEvent?(.status(st))
        case KISS.cmdData:
            onEvent?(.frame(ReceivedFrame(raw: f.payload, status: pendingRX, receivedAt: Date())))
            pendingRX = nil
        case KISS.cmdAckMode where f.payload.count >= 2:
            onEvent?(.ack(UInt16(f.payload[0]) << 8 | UInt16(f.payload[1])))
        default:
            onEvent?(.kiss(f))
        }
    }

    // MARK: writing (serialised on a background queue; never blocks the caller)

    public func writeRaw(_ bytes: [UInt8]) {
        writeQueue.async { [port, weak self] in
            do { try port.write(bytes) } catch {
                self?.onEvent?(.disconnected("\(error)"))
            }
        }
    }

    public func send(_ ax25: [UInt8]) { writeRaw(KISS.data(ax25)) }
    public func send(_ ax25: [UInt8], ackSeq: UInt16) { writeRaw(KISS.ackMode(seq: ackSeq, ax25)) }
    public func setHardware(_ line: String) { writeRaw(KISS.setHardware(line)) }
}

/// Tiny lock-based flag (Foundation-only, works on Linux too).
final class AtomicFlag {
    private let lock = NSLock()
    private var value = false
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
    func set() { lock.lock(); value = true; lock.unlock() }
}
