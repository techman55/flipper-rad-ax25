import Foundation

/// Finds Flipper serial ports and picks the TNC channel.
///
/// With the KISS TNC app running, the Flipper is a dual-CDC device with ACM
/// functions on USB interfaces 0 (CLI) and 2 (TNC). macOS names them
/// `/dev/cu.usbmodemflip_<Name>1` and `/dev/cu.usbmodemflip_<Name>3`
/// (trailing digit = interface + 1). On Linux they are ttyACMn, and sysfs
/// tells us the interface number.
public struct SerialPortInfo: Hashable, Identifiable {
    public let path: String
    public let interface: Int?
    public let isTNC: Bool
    public var id: String { path }

    public var displayName: String {
        let name = (path as NSString).lastPathComponent
        return isTNC ? "\(name)  (TNC)" : (interface == 0 ? "\(name)  (CLI)" : name)
    }
}

public enum PortScanner {
    public static func scan(devDir: String = "/dev") -> [SerialPortInfo] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: devDir)) ?? []
        #if os(Linux)
        let raw: [(String, Int?)] = names.filter { $0.hasPrefix("ttyACM") }.map { name in
            let p = "/sys/class/tty/\(name)/device/bInterfaceNumber"
            let iface = (try? String(contentsOfFile: p, encoding: .utf8))
                .flatMap { Int($0.trimmingCharacters(in: .whitespacesAndNewlines), radix: 16) }
            return ("\(devDir)/\(name)", iface)
        }
        #else
        let raw: [(String, Int?)] = names.filter { $0.hasPrefix("cu.usbmodemflip") }.map { name in
            ("\(devDir)/\(name)", name.last.flatMap { Int(String($0)) }.map { $0 - 1 })
        }
        #endif
        return classify(raw)
    }

    /// Within each device (same name minus the trailing digit), the highest interface is the TNC.
    public static func classify(_ ports: [(path: String, interface: Int?)]) -> [SerialPortInfo] {
        var groups: [String: [(String, Int?)]] = [:]
        for p in ports {
            let key = p.path.hasPrefix("/dev/ttyACM") ? "acm" : String(p.path.dropLast())
            groups[key, default: []].append((p.path, p.interface))
        }
        var out: [SerialPortInfo] = []
        for (_, group) in groups {
            let ifaces = Set(group.compactMap { $0.1 })
            let top = ifaces.count >= 2 ? ifaces.max() : nil
            for (path, iface) in group {
                out.append(SerialPortInfo(path: path, interface: iface, isTNC: top != nil && iface == top))
            }
        }
        return out.sorted { ($0.isTNC ? 0 : 1, $0.path) < ($1.isTNC ? 0 : 1, $1.path) }
    }

    public static func bestTNCPort() -> SerialPortInfo? {
        scan().first { $0.isTNC }
    }
}
