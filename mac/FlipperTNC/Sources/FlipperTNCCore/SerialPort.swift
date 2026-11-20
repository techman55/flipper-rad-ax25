import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

#if canImport(Darwin)
private let sysRead = Darwin.read
private let sysWrite = Darwin.write
private let sysClose = Darwin.close
#else
private let sysRead = Glibc.read
private let sysWrite = Glibc.write
private let sysClose = Glibc.close
#endif

public enum SerialError: Error, CustomStringConvertible {
    case open(String, Int32)
    case configure(Int32)
    case io(Int32)
    case closed

    public var description: String {
        switch self {
        case .open(let path, let e): return "open \(path): \(String(cString: strerror(e)))"
        case .configure(let e): return "configure: \(String(cString: strerror(e)))"
        case .io(let e): return "I/O: \(String(cString: strerror(e)))"
        case .closed: return "port closed"
        }
    }
}

/// Raw serial port over POSIX termios. Baud is irrelevant for USB CDC but set to 115200 anyway.
/// Opening a tty asserts DTR, which the Flipper uses to show "USB: yes".
public final class SerialPort {
    public let path: String
    private var fd: Int32
    private let lock = NSLock()

    public init(path: String) throws {
        self.path = path
        fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { throw SerialError.open(path, errno) }

        var t = termios()
        guard tcgetattr(fd, &t) == 0 else {
            let e = errno; _ = sysClose(fd); throw SerialError.configure(e)
        }
        cfmakeraw(&t)
        cfsetspeed(&t, speed_t(B115200))
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        guard tcsetattr(fd, TCSANOW, &t) == 0 else {
            let e = errno; _ = sysClose(fd); throw SerialError.configure(e)
        }
        tcflush(fd, TCIOFLUSH)
    }

    deinit { close() }

    public var isOpen: Bool { lock.lock(); defer { lock.unlock() }; return fd >= 0 }

    /// Waits up to `timeoutMs` for data. Returns [] on timeout; throws when the device is gone.
    public func read(timeoutMs: Int32 = 200, maxBytes: Int = 1024) throws -> [UInt8] {
        let f = currentFD()
        guard f >= 0 else { throw SerialError.closed }
        var pfd = pollfd(fd: f, events: Int16(POLLIN), revents: 0)
        let r = poll(&pfd, 1, timeoutMs)
        if r == 0 { return [] }
        if r < 0 {
            if errno == EINTR { return [] }
            throw SerialError.io(errno)
        }
        if pfd.revents & Int16(POLLHUP | POLLERR | POLLNVAL) != 0 { throw SerialError.closed }
        var buf = [UInt8](repeating: 0, count: maxBytes)
        let n = buf.withUnsafeMutableBytes { sysRead(f, $0.baseAddress, maxBytes) }
        if n > 0 { return Array(buf[0..<n]) }
        if n == 0 { throw SerialError.closed }  // EOF: unplugged
        if errno == EAGAIN || errno == EINTR { return [] }
        throw SerialError.io(errno)
    }

    public func write(_ bytes: [UInt8]) throws {
        var off = 0
        while off < bytes.count {
            let f = currentFD()
            guard f >= 0 else { throw SerialError.closed }
            let n = bytes[off...].withUnsafeBytes { sysWrite(f, $0.baseAddress, $0.count) }
            if n > 0 { off += n; continue }
            if n < 0 && (errno == EAGAIN || errno == EINTR) {
                var pfd = pollfd(fd: f, events: Int16(POLLOUT), revents: 0)
                _ = poll(&pfd, 1, 500)
                continue
            }
            throw SerialError.io(errno)
        }
    }

    public func close() {
        lock.lock()
        defer { lock.unlock() }
        if fd >= 0 {
            _ = sysClose(fd)
            fd = -1
        }
    }

    private func currentFD() -> Int32 {
        lock.lock(); defer { lock.unlock() }
        return fd
    }
}
