import XCTest
@testable import FlipperTNCCore

final class KISSTests: XCTestCase {
    func testEscapeVector() {
        let payload: [UInt8] = [0x01, 0xC0, 0x02, 0xDB, 0xDC, 0xDD, 0xC0, 0xDB]
        XCTAssertEqual(KISS.encode(type: 0, payload: payload),
                       [0xC0, 0x00, 0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0xDC, 0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0])
    }

    func testRoundTripChunked() {
        var rng = SystemRandomNumberGenerator()
        var dec = KISSDecoder()
        for _ in 0..<300 {
            let payload = (0..<Int.random(in: 1...400, using: &rng)).map { _ in UInt8.random(in: 0...255, using: &rng) }
            let type = UInt8.random(in: 0...15, using: &rng)
            let stream = KISS.encode(type: type, payload: payload)
            var frames: [KISSFrame] = []
            var i = 0
            while i < stream.count {
                let n = min(Int.random(in: 1...64, using: &rng), stream.count - i)
                frames += dec.feed(stream[i..<i + n])
                i += n
            }
            XCTAssertEqual(frames, [KISSFrame(type: type, payload: payload)])
        }
    }

    func testAckModeAndSetHardware() {
        XCTAssertEqual(KISS.ackMode(seq: 0x1234, [0x58]), [0xC0, 0x0C, 0x12, 0x34, 0x58, 0xC0])
        XCTAssertEqual(KISS.setHardware("GET"), [0xC0, 0x06, 0x47, 0x45, 0x54, 0xC0])
        var dec = KISSDecoder()
        let f = dec.feed(Array("junk".utf8) + KISS.data([1, 2]) + [0xC0, 0xC0] + KISS.ackMode(seq: 5, []))
        XCTAssertEqual(f.map(\.command), [0x00, 0x0C])
    }
}

// Same bytes as fap/modem ax25_build_ui() and the Python package.
let uiHello: [UInt8] = [0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82,
                        0x98, 0x98, 0x61, 0x03, 0xF0] + Array("hello".utf8)

final class AX25Tests: XCTestCase {

    func testBuildMatchesDevice() throws {
        let f = try AX25Frame.ui(source: "N0CALL", destination: "TEST", text: "hello")
        XCTAssertEqual(try f.encode(), uiHello)
    }

    func testDecodeWithPath() throws {
        let raw = try AX25Frame.ui(source: "kn4xxx-1", destination: "APRS", path: "WIDE1-1, WIDE2-2",
                                   text: "!4903.50N/07201.75W-").encode()
        var f = try AX25Frame.decode(raw)
        XCTAssertEqual(f.source.description, "KN4XXX-1")
        XCTAssertEqual(f.path.map(\.description), ["WIDE1-1", "WIDE2-2"])
        XCTAssertEqual(f.pid, 0xF0)
        f.path[0].hBit = true
        XCTAssertEqual(f.description, "KN4XXX-1>APRS,WIDE1-1*,WIDE2-2: !4903.50N/07201.75W-")
        XCTAssertEqual(f.route, "KN4XXX-1 → APRS via WIDE1-1*,WIDE2-2")
    }

    func testBadInput() {
        for bad in ["", "TOOLONG1", "BAD-16", "A*B", "AB-"] {
            XCTAssertThrowsError(try AX25Address(parsing: bad), bad)
        }
        XCTAssertThrowsError(try AX25Frame.decode([0, 0, 0]))
        XCTAssertThrowsError(try AX25Frame.ui(source: "A", destination: "B",
                                              text: String(repeating: "x", count: 400)).encode())
    }

    func testFCSVectors() {
        XCTAssertEqual(ax25FCS(Array("123456789".utf8)), 0x906E)
        XCTAssertEqual(ax25FCS([0xA8, 0x8A, 0xA6, 0xA8, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82,
                                0x98, 0x98, 0x61, 0x03, 0xF0, 0x68, 0x69]), 0x58CF)
    }

    func testHex() {
        XCTAssertEqual([UInt8](hex: "c0 DB 00"), [0xC0, 0xDB, 0x00])
        XCTAssertNil([UInt8](hex: "abc"))
        XCTAssertEqual([UInt8]([1, 0xAB]).hexString, "01ab")
    }
}

final class StatusAndPortTests: XCTestCase {
    func testStatusParsing() {
        let rx = StatusLine("RX RSSI=-87 FREQ=433920000 MODE=0 FCS=OK T=12345")
        XCTAssertEqual(rx.kind, "RX")
        XCTAssertEqual(rx.int("RSSI"), -87)
        XCTAssertFalse(rx.isReply)
        let err = StatusLine("ERR TX_BLOCKED_REGION SEQ=3")
        XCTAssertEqual(err.errorCode, "TX_BLOCKED_REGION")
        XCTAssertFalse(err.isReply)  // tied to a frame, not to a SetHardware request
        XCTAssertTrue(StatusLine("ERR FREQ_RANGE").isReply)
        XCTAssertTrue(StatusLine("OK FREQ=433920000 TXOK=1").isReply)
    }

    func testPortClassification() {
        let ports = PortScanner.classify([
            (path: "/dev/cu.usbmodemflip_Anana1", interface: 0),
            (path: "/dev/cu.usbmodemflip_Anana3", interface: 2),
        ])
        XCTAssertEqual(ports.first?.path, "/dev/cu.usbmodemflip_Anana3")
        XCTAssertEqual(ports.first?.isTNC, true)
        XCTAssertEqual(PortScanner.classify([(path: "/dev/cu.usbmodemflip_X1", interface: 0)]).first?.isTNC, false)
    }

    /// End-to-end through a pseudo-terminal: TNCConnection pairs RX status with the data frame,
    /// parses ACKs, and writes what we send.
    func testConnectionOverPTY() throws {
        var master: Int32 = -1, slave: Int32 = -1
        var name = [CChar](repeating: 0, count: 128)
        XCTAssertEqual(openpty(&master, &slave, &name, nil, nil), 0)
        defer { close(master); close(slave) }
        let path = String(cString: name)

        let conn = try TNCConnection(path: path)
        let got = expectation(description: "frame + ack")
        got.expectedFulfillmentCount = 2
        var frame: ReceivedFrame?
        var ack: UInt16?
        conn.onEvent = { ev in
            switch ev {
            case .frame(let f): frame = f; got.fulfill()
            case .ack(let s): ack = s; got.fulfill()
            default: break
            }
        }
        conn.start()
        defer { conn.close() }

        let device = KISS.setHardware("RX RSSI=-91 FREQ=433920000 MODE=0 FCS=OK T=5")
            + KISS.data(uiHello) + KISS.encode(type: KISS.cmdAckMode, payload: [0x00, 0x07])
        _ = device.withUnsafeBytes { write(master, $0.baseAddress, $0.count) }
        wait(for: [got], timeout: 3)
        XCTAssertEqual(frame?.raw, uiHello)
        XCTAssertEqual(frame?.rssi, -91)
        XCTAssertEqual(frame?.decoded?.description, "N0CALL>TEST: hello")
        XCTAssertEqual(ack, 7)

        conn.send(uiHello, ackSeq: 9)
        var buf = [UInt8](repeating: 0, count: 256)
        var received: [UInt8] = []
        let deadline = Date().addingTimeInterval(2)
        while received.count < 4 + uiHello.count && Date() < deadline {
            let n = buf.withUnsafeMutableBytes { read(master, $0.baseAddress, 256) }
            if n > 0 { received += buf[0..<n] }
        }
        var dec = KISSDecoder()
        let frames = dec.feed(received)
        XCTAssertEqual(frames.first?.command, KISS.cmdAckMode)
        XCTAssertEqual(Array(frames.first?.payload.prefix(2) ?? []), [0x00, 0x09])
    }
}
