import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
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
