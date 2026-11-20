// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "FlipperTNC",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "FlipperTNC", targets: ["FlipperTNC"]),
        .library(name: "FlipperTNCCore", targets: ["FlipperTNCCore"]),
    ],
    targets: [
        // Protocol + serial I/O. Foundation/POSIX only, so it also builds and tests on Linux.
        .target(name: "FlipperTNCCore"),
        // SwiftUI app (macOS). On other platforms it compiles to a stub.
        .executableTarget(name: "FlipperTNC", dependencies: ["FlipperTNCCore"]),
        .testTarget(name: "FlipperTNCCoreTests", dependencies: ["FlipperTNCCore"]),
    ],
    swiftLanguageVersions: [.v5]
)
