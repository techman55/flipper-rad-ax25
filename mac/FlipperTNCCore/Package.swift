// swift-tools-version:5.9
import PackageDescription

// Protocol + serial I/O for the Flipper TNC Mac app (KISS, AX.25, status lines,
// termios serial port, port scanner, KISS-TCP server). Foundation/POSIX only, so
// `swift test` also runs on Linux. The app itself is ../FlipperTNC.xcodeproj.
let package = Package(
    name: "FlipperTNCCore",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "FlipperTNCCore", targets: ["FlipperTNCCore"]),
    ],
    targets: [
        .target(name: "FlipperTNCCore"),
        .testTarget(name: "FlipperTNCCoreTests", dependencies: ["FlipperTNCCore"]),
    ],
    swiftLanguageVersions: [.v5]
)
