import AppKit
import FlipperTNCCore
import Foundation
import Observation

struct LogEntry: Identifiable, Hashable {
    enum Direction: String { case rx = "RX", tx = "TX", status = "··" }
    let id = UUID()
    let time: Date
    let direction: Direction
    let rssi: Int?
    let route: String
    let info: String
    let hex: String
    let fcs: String

    var jsonObject: [String: Any] {
        var o: [String: Any] = [
            "time": ISO8601DateFormatter.withFractional.string(from: time),
            "dir": direction == .status ? "status" : direction.rawValue.lowercased(),
            "route": route, "info": info, "hex": hex, "fcs": fcs,
        ]
        if let rssi { o["rssi"] = rssi }
        return o
    }

    var textLine: String {
        let r = rssi.map { String(format: "%4d dBm", $0) } ?? "        "
        return "\(DateFormatter.logTime.string(from: time))  \(direction.rawValue)  \(r)  \(route)  \(info)  [\(fcs)]"
    }
}

struct Outgoing: Identifiable {
    enum State: Equatable { case queued, sent, failed(String) }
    let id = UUID()
    let seq: UInt16
    let summary: String
    let time: Date
    var state: State
}

extension DateFormatter {
    static let logTime: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()
}

extension ISO8601DateFormatter {
    static let withFractional: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()
}

@MainActor
@Observable
final class AppModel {
    // MARK: connection
    var ports: [SerialPortInfo] = []
    var selectedPath: String? = nil
    var autoConnect = true
    private(set) var connectedPath: String? = nil
    var connectionError: String? = nil
    var isConnected: Bool { connectedPath != nil }
    @ObservationIgnored private var connection: TNCConnection?
    @ObservationIgnored private var timer: Timer?

    // MARK: device status
    var deviceState = "—"
    var rssi: Int? = nil
    var lastHeartbeat: Date? = nil
    var rxCount = 0
    var txCount = 0

    // MARK: settings form
    var freqMHz = "433.920"
    var mode = 0
    var modulation = "GFSK"
    var power = 10
    var radio = "INT"
    var squelch = -95
    var callsign: String = UserDefaults.standard.string(forKey: "callsign") ?? "N0CALL" {
        didSet { UserDefaults.standard.set(callsign, forKey: "callsign") }
    }
    var lastReply: String? = nil
    var lastReplyIsError = false
    @ObservationIgnored private var awaitingGet = false

    // MARK: compose
    var composeDest = "TEST"
    var composePath = ""
    var composeText = ""
    var composeHex = ""
    var composeUseHex = false
    var composeError: String? = nil
    var outgoing: [Outgoing] = []
    @ObservationIgnored private var nextSeq: UInt16 = UInt16.random(in: 1...0x7FFF)

    // MARK: log
    var log: [LogEntry] = []
    var search = ""
    var showHex = false
    var showStatusLines = false
    let maxLogEntries = 5000

    // MARK: TCP
    var tcpEnabled = false {
        didSet { if tcpEnabled != oldValue { updateTCP() } }
    }
    var tcpClients = 0
    var tcpError: String? = nil
    @ObservationIgnored private var tcpServer: KissTCPServer?

    static let modes = [(0, "FSK 9600 (G3RUH)"), (1, "AFSK 1200 (Bell 202)"), (2, "FSK 1200")]
    static let powers = [-10, 0, 5, 10]

    init() {
        refreshPorts()
        // Port scan + auto-(re)connect after a replug.
        timer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    var filteredLog: [LogEntry] {
        log.filter { e in
            (showStatusLines || e.direction != .status) &&
                (search.isEmpty || e.route.localizedCaseInsensitiveContains(search) ||
                    e.info.localizedCaseInsensitiveContains(search) || e.hex.contains(search.lowercased()))
        }
    }

    // MARK: - connection management

    func refreshPorts() {
        ports = PortScanner.scan()
        if selectedPath == nil || !ports.contains(where: { $0.path == selectedPath }) {
            selectedPath = ports.first(where: { $0.isTNC })?.path ?? ports.first?.path
        }
    }

    private func tick() {
        refreshPorts()
        if !isConnected, autoConnect, let tnc = ports.first(where: { $0.isTNC }) {
            connect(path: tnc.path)
        }
    }

    func connect(path: String? = nil) {
        guard let path = path ?? selectedPath else { return }
        disconnect()
        do {
            let c = try TNCConnection(path: path)
            c.onEvent = { [weak self] ev in Task { @MainActor in self?.handle(ev) } }
            c.onRawFrame = { [weak self] f in
                Task { @MainActor in self?.tcpServer?.broadcast(f) }
            }
            c.start()
            connection = c
            connectedPath = path
            selectedPath = path
            connectionError = nil
            requestGet()
        } catch {
            connectionError = "\(error)"
        }
    }

    func disconnect() {
        connection?.close()
        connection = nil
        connectedPath = nil
        deviceState = "—"
    }

    // MARK: - events

    private func handle(_ ev: TNCEvent) {
        switch ev {
        case .frame(let rx):
            rxCount += 1
            let f = rx.decoded
            append(LogEntry(time: rx.receivedAt, direction: .rx, rssi: rx.rssi,
                            route: f?.route ?? "(undecodable)", info: f?.infoText ?? "",
                            hex: rx.raw.hexString, fcs: rx.fcsOK ? "OK" : "BAD"))
        case .status(let st):
            handleStatus(st)
        case .ack(let seq):
            mark(seq: seq, state: .sent)
        case .kiss:
            break
        case .disconnected(let why):
            connectionError = "Disconnected: \(why)"
            disconnect()
        }
    }

    private func handleStatus(_ st: StatusLine) {
        switch st.kind {
        case "RX":
            rssi = st.int("RSSI")
            return  // shown with the frame that follows
        case "HB":
            lastHeartbeat = Date()
            deviceState = st.fields["STATE"] ?? deviceState
            rssi = st.int("RSSI") ?? rssi
        case "TXDONE":
            txCount += 1
            if let seq = st.int("SEQ") { mark(seq: UInt16(truncatingIfNeeded: seq), state: .sent) }
        case "ERR":
            if let seq = st.int("SEQ") {
                mark(seq: UInt16(truncatingIfNeeded: seq), state: .failed(st.errorCode ?? "ERR"))
            }
        default:
            break
        }
        if st.isReply {
            lastReply = st.raw
            lastReplyIsError = st.kind == "ERR"
            if st.kind == "OK" && awaitingGet { applyGet(st) }
            awaitingGet = false
        }
        if st.kind == "CFG" { applyGet(st) }  // changed on the Flipper's own buttons
        append(LogEntry(time: Date(), direction: .status, rssi: nil, route: st.kind,
                        info: st.raw, hex: "", fcs: "—"))
    }

    private func applyGet(_ st: StatusLine) {
        if let f = st.int("FREQ") { freqMHz = String(format: "%.3f", Double(f) / 1e6) }
        if let m = st.int("MODE") { mode = m }
        if let m = st.fields["MOD"] { modulation = m }
        if let p = st.int("PWR") { power = p }
        if let r = st.fields["RADIO"] { radio = r }
        if let s = st.int("SQL") { squelch = s }
        if let c = st.fields["CALL"], c != "N0CALL" || callsign.isEmpty { callsign = c }
        if let s = st.fields["STATE"] { deviceState = s }
        if let r = st.int("RXF") { rxCount = r }
        if let t = st.int("TXF") { txCount = t }
    }

    private func append(_ e: LogEntry) {
        log.append(e)
        if log.count > maxLogEntries { log.removeFirst(log.count - maxLogEntries) }
    }

    private func mark(seq: UInt16, state: Outgoing.State) {
        guard let i = outgoing.lastIndex(where: { $0.seq == seq }) else { return }
        if case .failed = outgoing[i].state { return }
        outgoing[i].state = state
    }

    // MARK: - actions

    func requestGet() {
        awaitingGet = true
        connection?.setHardware("GET")
    }

    func applySettings() {
        guard let mhz = Double(freqMHz.replacingOccurrences(of: ",", with: ".")) else {
            lastReply = "ERR bad frequency"
            lastReplyIsError = true
            return
        }
        let hz = Int((mhz * 1e6).rounded())
        let call = callsign.trimmingCharacters(in: .whitespaces).uppercased()
        var line = "FREQ=\(hz);MODE=\(mode);MOD=\(modulation);PWR=\(power);RADIO=\(radio);SQL=\(squelch)"
        if (try? AX25Address(parsing: call)) != nil { line += ";CALL=\(call)" }
        lastReply = "…"
        lastReplyIsError = false
        connection?.setHardware(line)
    }

    func send() {
        composeError = nil
        guard let c = connection else { composeError = "Not connected"; return }
        let bytes: [UInt8]
        let summary: String
        do {
            if composeUseHex {
                guard let b = [UInt8](hex: composeHex), !b.isEmpty else { throw AX25Error.malformed("bad hex") }
                bytes = b
                summary = (try? AX25Frame.decode(b).description) ?? b.hexString
            } else {
                let f = try AX25Frame.ui(source: callsign, destination: composeDest,
                                         path: composePath, text: composeText)
                bytes = try f.encode()
                summary = f.description
            }
        } catch {
            composeError = "\(error)"
            return
        }
        let seq = nextSeq
        nextSeq = nextSeq &+ 1
        outgoing.append(Outgoing(seq: seq, summary: summary, time: Date(), state: .queued))
        if outgoing.count > 50 { outgoing.removeFirst() }
        c.send(bytes, ackSeq: seq)
        let decoded = try? AX25Frame.decode(bytes)
        append(LogEntry(time: Date(), direction: .tx, rssi: nil, route: decoded?.route ?? "(raw)",
                        info: decoded?.infoText ?? "", hex: bytes.hexString, fcs: "—"))
        // Flag frames that never get an ACK (radio busy for > 10 s reports its own error).
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(30))
            if let self, let i = self.outgoing.firstIndex(where: { $0.seq == seq }),
               self.outgoing[i].state == .queued {
                self.outgoing[i].state = .failed("no ack")
            }
        }
    }

    func clearLog() { log.removeAll() }

    // MARK: - export

    func export(jsonl: Bool) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "flipper-tnc-log." + (jsonl ? "jsonl" : "txt")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let entries = filteredLog
        let text: String
        if jsonl {
            text = entries.compactMap { e in
                (try? JSONSerialization.data(withJSONObject: e.jsonObject, options: [.sortedKeys]))
                    .map { String(decoding: $0, as: UTF8.self) }
            }.joined(separator: "\n") + "\n"
        } else {
            text = entries.map(\.textLine).joined(separator: "\n") + "\n"
        }
        do { try text.write(to: url, atomically: true, encoding: .utf8) } catch {
            connectionError = "Export failed: \(error.localizedDescription)"
        }
    }

    // MARK: - TCP bridge

    private func updateTCP() {
        if tcpEnabled {
            let server = KissTCPServer(port: 8001) { [weak self] bytes in
                Task { @MainActor in self?.connection?.writeRaw(bytes) }
            }
            server.onClientCountChange = { [weak self] n in Task { @MainActor in self?.tcpClients = n } }
            do {
                try server.start()
                tcpServer = server
                tcpError = nil
            } catch {
                tcpError = "\(error)"
                tcpEnabled = false
            }
        } else {
            tcpServer?.stop()
            tcpServer = nil
            tcpClients = 0
        }
    }
}
