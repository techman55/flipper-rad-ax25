#if canImport(Network)
import Foundation
import Network

/// KISS-over-TCP server (default port 8001) so other packet apps can share the Flipper.
/// Client bytes are decoded into whole KISS frames before being written to the radio,
/// so two clients can never interleave partial frames. Radio frames go to every client.
public final class KissTCPServer {
    public let port: UInt16
    public var forwardStatus = false
    public private(set) var clientCount = 0
    public var onClientCountChange: ((Int) -> Void)?

    private let toRadio: ([UInt8]) -> Void
    private var listener: NWListener?
    private var clients: [ObjectIdentifier: NWConnection] = [:]
    private let queue = DispatchQueue(label: "FlipperTNC.tcp")

    public init(port: UInt16 = 8001, toRadio: @escaping ([UInt8]) -> Void) {
        self.port = port
        self.toRadio = toRadio
    }

    public func start() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let l = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        l.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        l.start(queue: queue)
        listener = l
    }

    public func stop() {
        queue.async { [self] in
            listener?.cancel()
            listener = nil
            clients.values.forEach { $0.cancel() }
            clients.removeAll()
            publishCount()
        }
    }

    /// Call with every KISS frame received from the radio.
    public func broadcast(_ frame: KISSFrame) {
        if frame.command == KISS.cmdSetHardware && !forwardStatus { return }
        let bytes = Data(KISS.encode(type: frame.type, payload: frame.payload))
        queue.async { [self] in
            for c in clients.values { c.send(content: bytes, completion: .contentProcessed { _ in }) }
        }
    }

    private func accept(_ conn: NWConnection) {
        let key = ObjectIdentifier(conn)
        clients[key] = conn
        publishCount()
        var decoder = KISSDecoder()
        func receive() {
            conn.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, done, err in
                guard let self else { return }
                if let data, !data.isEmpty {
                    for f in decoder.feed(data) {
                        self.toRadio(KISS.encode(type: f.type, payload: f.payload))
                    }
                }
                if done || err != nil {
                    conn.cancel()
                    self.clients[key] = nil
                    self.publishCount()
                } else {
                    receive()
                }
            }
        }
        conn.start(queue: queue)
        receive()
    }

    private func publishCount() {
        clientCount = clients.count
        let n = clientCount
        onClientCountChange?(n)
    }
}
#endif
