#if canImport(SwiftUI) && os(macOS)
import FlipperTNCCore
import SwiftUI

struct ContentView: View {
    @Bindable var model: AppModel

    var body: some View {
        HSplitView {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    ConnectionSection(model: model)
                    Divider()
                    SettingsSection(model: model)
                    Divider()
                    ComposeSection(model: model)
                }
                .padding()
            }
            .frame(minWidth: 320, idealWidth: 360, maxWidth: 460)

            LogSection(model: model)
                .frame(minWidth: 520)
        }
        .safeAreaInset(edge: .bottom, spacing: 0) { StatusBar(model: model) }
    }
}

// MARK: - Connection

struct ConnectionSection: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Connection").font(.headline)
            HStack {
                Picker("Port", selection: $model.selectedPath) {
                    if model.ports.isEmpty { Text("No Flipper found").tag(String?.none) }
                    ForEach(model.ports) { p in Text(p.displayName).tag(Optional(p.path)) }
                }
                .labelsHidden()
                Button {
                    model.refreshPorts()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .help("Rescan ports")
            }
            HStack {
                if model.isConnected {
                    Button("Disconnect") {
                        model.autoConnect = false
                        model.disconnect()
                    }
                } else {
                    Button("Connect") {
                        model.autoConnect = true
                        model.connect()
                    }
                    .disabled(model.selectedPath == nil)
                }
                Toggle("Auto-reconnect", isOn: $model.autoConnect)
            }
            if let err = model.connectionError {
                Text(err).font(.caption).foregroundStyle(.red)
            }
        }
    }
}

// MARK: - Settings

struct SettingsSection: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Radio settings").font(.headline)
            Form {
                TextField("Frequency (MHz)", text: $model.freqMHz)
                Picker("Mode", selection: $model.mode) {
                    ForEach(AppModel.modes, id: \.0) { m in Text(m.1).tag(m.0) }
                }
                Picker("Power", selection: $model.power) {
                    ForEach(AppModel.powers, id: \.self) { p in Text("\(p) dBm").tag(p) }
                }
                Picker("Radio", selection: $model.radio) {
                    Text("Internal CC1101").tag("INT")
                    Text("External CC1101").tag("EXT")
                }
                Stepper("Squelch: \(model.squelch) dBm", value: $model.squelch, in: -140...(-20), step: 1)
                TextField("My callsign", text: $model.callsign)
            }
            HStack {
                Button("Apply") { model.applySettings() }
                    .keyboardShortcut(.return, modifiers: [.command])
                    .disabled(!model.isConnected)
                Button("Read from device") { model.requestGet() }
                    .disabled(!model.isConnected)
            }
            if let reply = model.lastReply {
                Text(reply)
                    .font(.caption.monospaced())
                    .foregroundStyle(model.lastReplyIsError ? Color.red : Color.secondary)
                    .textSelection(.enabled)
                    .lineLimit(4)
            }
        }
    }
}

// MARK: - Compose

struct ComposeSection: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Send").font(.headline)
            Picker("", selection: $model.composeUseHex) {
                Text("Text").tag(false)
                Text("Raw hex").tag(true)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            if model.composeUseHex {
                TextField("AX.25 frame as hex (no FCS)", text: $model.composeHex, axis: .vertical)
                    .font(.body.monospaced())
                    .lineLimit(3...6)
            } else {
                Form {
                    LabeledContent("Source") { Text(model.callsign).monospaced() }
                    TextField("Destination", text: $model.composeDest)
                    TextField("Path", text: $model.composePath, prompt: Text("e.g. WIDE1-1"))
                    TextField("Text", text: $model.composeText, axis: .vertical)
                        .lineLimit(1...4)
                }
            }
            HStack {
                Button("Send") { model.send() }
                    .keyboardShortcut("s", modifiers: [.command])
                    .disabled(!model.isConnected)
                if let err = model.composeError {
                    Text(err).font(.caption).foregroundStyle(.red)
                }
            }
            ForEach(model.outgoing.suffix(6).reversed()) { o in
                HStack(spacing: 6) {
                    switch o.state {
                    case .queued:
                        Image(systemName: "clock").foregroundStyle(.orange)
                    case .sent:
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                    case .failed:
                        Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
                    }
                    Text(o.summary).lineLimit(1).truncationMode(.tail)
                    Spacer()
                    Text(stateText(o.state)).font(.caption).foregroundStyle(.secondary)
                }
                .font(.caption)
            }
        }
    }

    private func stateText(_ s: Outgoing.State) -> String {
        switch s {
        case .queued: return "queued"
        case .sent: return "sent ✓"
        case .failed(let why): return why
        }
    }
}

// MARK: - Log

struct LogSection: View {
    @Bindable var model: AppModel

    var body: some View {
        Table(model.filteredLog) {
            TableColumn("Time") { e in
                Text(DateFormatter.logTime.string(from: e.time)).monospacedDigit()
            }
            .width(min: 80, ideal: 90, max: 110)
            TableColumn("Dir") { e in
                Text(e.direction.rawValue)
                    .foregroundStyle(e.direction == .tx ? Color.orange : e.direction == .rx ? Color.blue : Color.secondary)
            }
            .width(32)
            TableColumn("RSSI") { e in
                Text(e.rssi.map { "\($0)" } ?? "").monospacedDigit()
            }
            .width(44)
            TableColumn("Route") { e in Text(e.route).lineLimit(1) }
                .width(min: 120, ideal: 200)
            TableColumn("Info") { e in
                Text(model.showHex && !e.hex.isEmpty ? e.hex : e.info)
                    .font(model.showHex ? Font.body.monospaced() : Font.body)
                    .lineLimit(1)
                    .textSelection(.enabled)
            }
            TableColumn("FCS") { e in
                Text(e.fcs).foregroundStyle(e.fcs == "BAD" ? Color.red : Color.secondary)
            }
            .width(36)
        }
        .searchable(text: $model.search, prompt: "Filter callsign, text or hex")
        .toolbar {
            ToolbarItemGroup {
                Toggle("Hex", isOn: $model.showHex)
                Toggle("Status lines", isOn: $model.showStatusLines)
                Toggle("KISS TCP :8001", isOn: $model.tcpEnabled)
                    .help("Share the Flipper with other packet apps over KISS-over-TCP")
                Menu("Export") {
                    Button("JSON Lines (.jsonl)…") { model.export(jsonl: true) }
                    Button("Text (.txt)…") { model.export(jsonl: false) }
                }
                Button("Clear") { model.clearLog() }
            }
        }
    }
}

// MARK: - Status bar

struct StatusBar: View {
    @Bindable var model: AppModel

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            HStack(spacing: 14) {
                Label(model.isConnected ? "Connected" : "Disconnected",
                      systemImage: model.isConnected ? "cable.connector" : "cable.connector.slash")
                    .foregroundStyle(model.isConnected ? Color.green : Color.red)
                Text("State: \(model.deviceState)")
                Text("Heartbeat: \(heartbeatAge(now: context.date))")
                if let r = model.rssi { Text("RSSI: \(r) dBm") }
                Text("RX \(model.rxCount)  TX \(model.txCount)")
                if model.tcpEnabled { Text("TCP clients: \(model.tcpClients)") }
                if let e = model.tcpError { Text(e).foregroundStyle(.red) }
                Spacer()
                if let p = model.connectedPath { Text((p as NSString).lastPathComponent).foregroundStyle(.secondary) }
            }
            .font(.caption)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(.bar)
        }
    }

    private func heartbeatAge(now: Date) -> String {
        guard let hb = model.lastHeartbeat else { return "—" }
        let s = Int(now.timeIntervalSince(hb))
        return s < 1 ? "now" : "\(s)s ago"
    }
}
#endif
