import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor weak var model: AppModel?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, model.phase == .building else { return .terminateNow }
        let a = NSAlert()
        a.messageText = String(localized: "A build is still running")
        a.informativeText = String(localized: "If you quit now the build is cancelled and nothing is saved.")
        a.addButton(withTitle: String(localized: "Keep Building")); a.addButton(withTitle: String(localized: "Cancel Build and Quit"))
        if a.runModal() == .alertFirstButtonReturn { return .terminateCancel }
        model.cancelBuild()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        Task { @MainActor in await model?.handleDrop(urls) }
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
                .onAppear { delegate.model = model; model.refreshLibrary(adoptOutput: true); SettingsDebug.openIfRequested() }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in model.refreshLibrary() }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: Theme.windowSize.width, height: Theme.windowSize.height)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Add Game…") { model.pickFilesAndAdd() }.keyboardShortcut("o").disabled(model.sets.isEmpty)
                Divider()
                Button("Build") { model.startBuild() }.keyboardShortcut("b").disabled(!model.canBuild)
                Button("Cancel Build") { model.cancelBuild() }.keyboardShortcut(".").disabled(!model.isBuilding)
            }
            CommandMenu("Device") {
                Button("Refresh Devices") { Task { await model.refreshIOS() } }.keyboardShortcut("r")
                Button("Install .ipa or .app…") { model.installFile() }.disabled(model.selectedDevice == nil)
            }
            CommandGroup(replacing: .help) {
                Button("Show Build Logs in Finder") { model.showLogsInFinder() }
                Link("Napple Studio Guide", destination: URL(string: "https://napple.app/guides/studio")!)
            }
        }
        WindowGroup("Console", id: "console", for: UUID.self) { $id in
            if let id { ConsoleWindow(id: id).environment(model) }
        }
        .windowResizability(.contentSize)
        Settings { SettingsRoot().environment(model) }
    }
}
