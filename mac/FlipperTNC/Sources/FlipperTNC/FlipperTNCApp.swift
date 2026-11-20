#if canImport(SwiftUI) && os(macOS)
import AppKit
import SwiftUI

/// `swift run` starts a bundle-less executable, which macOS treats as a background
/// process. Make it a regular app so it gets a Dock icon, menu bar and key window.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct FlipperTNCApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var model = AppModel()

    var body: some Scene {
        WindowGroup("Flipper TNC") {
            ContentView(model: model)
                .frame(minWidth: 900, minHeight: 560)
        }
        .windowResizability(.contentMinSize)
    }
}
#else
// The app UI is SwiftUI/AppKit and only builds on macOS. On other platforms this
// target is a stub so `swift build` / `swift test` still cover FlipperTNCCore.
@main
enum FlipperTNCStub {
    static func main() {
        print("FlipperTNC is a macOS app. Build it on macOS 14+ with `swift run` or Xcode.")
    }
}
#endif
