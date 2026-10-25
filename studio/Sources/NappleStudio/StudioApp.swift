import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor weak var model: AppModel?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// Quitting while a build runs would leave a half-written app behind: ask first, then stop the build cleanly.
    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, model.phase == .building else { return .terminateNow }
        let a = NSAlert()
        a.messageText = "A build is still running"
        a.informativeText = "If you quit now the build is cancelled and nothing is saved. Keep Studio open until it finishes."
        a.addButton(withTitle: "Keep Building"); a.addButton(withTitle: "Cancel Build and Quit")
        if a.runModal() == .alertFirstButtonReturn { return .terminateCancel }
        model.cancelBuild()
        // give the CLI a moment to remove its staging folder (SIGTERM stops it between 8 MB chunks)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}

@main
struct StudioApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = AppModel()

    var body: some Scene {
        Window("Napple Studio", id: "main") {
            RootView()
                .environment(model)
                .onAppear {
                    delegate.model = model
                    model.offerMoveToApplications()
                    model.refreshLibrary(adoptOutput: true)
                }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
                    model.refreshLibrary()
                }
        }
        .windowStyle(.plain)
        .windowResizability(.contentSize)
        .defaultSize(width: Metrics.width, height: Metrics.height)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Add Game…") { model.pickFilesAndAdd() }.keyboardShortcut("o")
                Button("Show Library") { if model.phase != .building { model.resetPlanState(); model.screen = .library } }
                    .keyboardShortcut("l")
            }
            CommandGroup(replacing: .appSettings) {
                Button("Settings…") { model.openSettings() }.keyboardShortcut(",")
            }
        }
    }
}
