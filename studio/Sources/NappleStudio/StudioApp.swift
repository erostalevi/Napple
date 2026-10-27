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
        WindowGroup("Napple Studio", id: "main") {
            RootView()
                .environment(model)
                .onAppear { delegate.model = model; model.refreshLibrary(adoptOutput: true) }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
                    model.refreshLibrary()
                }
                .frame(minWidth: 860, minHeight: 560)
        }
        .defaultSize(width: 1080, height: 700)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Add Game…") { model.pickFilesAndAdd() }.keyboardShortcut("o")
                Divider()
                Button("Build") { model.startBuild() }.keyboardShortcut("b").disabled(!model.canBuild)
                Button("Cancel Build") { model.cancelBuild() }.keyboardShortcut(".").disabled(!model.isBuilding)
            }
            CommandMenu("Go") {
                ForEach(Array(Pane.allCases.enumerated()), id: \.element) { i, p in
                    Button(p.title) { model.pane = p }.keyboardShortcut(KeyEquivalent(Character("\(i + 1)")), modifiers: .command)
                }
            }
            CommandMenu("Device") {
                Button("Refresh Devices") { Task { await model.refreshIOS() } }.keyboardShortcut("r")
                Button("Install on Device…") { model.installFile() }.disabled(model.selectedDevice == nil)
            }
            CommandGroup(after: .toolbar) {
                Button(model.showInspector ? "Hide Log" : "Show Log") { model.showInspector.toggle() }.keyboardShortcut("l", modifiers: [.command, .option])
            }
            CommandGroup(replacing: .help) {
                Button("Show Build Logs in Finder") { model.showLogsInFinder() }
                Link("Napple Studio Guide", destination: URL(string: "https://napple.app/guides/studio")!)
            }
        }
        Settings { SettingsView().environment(model) }
    }
}
