import SwiftUI
import AppKit
import NappleStudioCore

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
                .onAppear {
                    delegate.model = model; model.refreshLibrary(adoptOutput: true); SettingsDebug.openIfRequested()
                    DebugTour.startIfRequested(model)
                }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in model.refreshLibrary() }
        }
        .defaultSize(width: Theme.windowSize.width, height: Theme.windowSize.height)
        .defaultLaunchBehavior(.presented)   // a first launch has nothing to restore: still open the window
        .windowToolbarStyle(.unified)
        .commands { StudioCommands(model: model) }
        Window("About Napple Studio", id: "about") { AboutView().environment(model) }
            .windowStyle(.hiddenTitleBar).windowResizability(.contentSize)
        WindowGroup("Console", id: "console", for: UUID.self) { $id in
            if let id { ConsoleWindow(id: id).environment(model) }
        }
        .defaultSize(width: 720, height: 520)
        Settings { SettingsRoot().environment(model) }
    }
}

/// The menu bar. Every command has a shortcut where it makes sense and is disabled when it cannot run.
struct StudioCommands: Commands {
    let model: AppModel
    @Environment(\.openWindow) private var openWindow

    private var hasSelection: Bool { model.selectedItem != nil }

    var body: some Commands {
        CommandGroup(replacing: .appInfo) {
            Button("About Napple Studio") { openWindow(id: "about") }
        }
        CommandGroup(replacing: .newItem) {
            Button("Add Game…") { model.pickFilesAndAdd() }.keyboardShortcut("o").disabled(model.isBuilding)
            Divider()
            Button("Build") { model.startBuild() }.keyboardShortcut("b").disabled(!model.canBuild)
            Button("Cancel Build") { model.cancelBuild() }.keyboardShortcut(".").disabled(!model.isBuilding)
        }
        CommandMenu("Library") {
            Button("Play") { model.launch(model.selectedItem?.url) }.keyboardShortcut(.return, modifiers: .command).disabled(!hasSelection)
            Button("Play with Debug") { if let i = model.selectedItem { model.playWithDebug(i) } }.keyboardShortcut(.return, modifiers: [.command, .option]).disabled(!hasSelection)
            Divider()
            Button("Show in Finder") { model.reveal(model.selectedItem?.url) }.keyboardShortcut("r", modifiers: [.command, .shift]).disabled(!hasSelection)
            Button("Open Saves Folder") { if let i = model.selectedItem { model.openSaves(i) } }.keyboardShortcut("s", modifiers: [.command, .shift]).disabled(!hasSelection)
            Button("Update or Add DLC…") { if let i = model.selectedItem { model.rebuildWithNewFiles(i) } }.disabled(!hasSelection || model.isBuilding)
            Button("Rebuild with Mods…") { if let i = model.selectedItem { model.rebuildWithMods(i) } }.disabled(!hasSelection || model.isBuilding)
            Divider()
            Button("Move to Trash") { if let i = model.selectedItem { model.trashFromLibrary(i) } }.keyboardShortcut(.delete, modifiers: .command).disabled(!hasSelection)
            Button("Remove from Library") { if let i = model.selectedItem { model.removeFromLibrary(i.id) } }.disabled(!hasSelection)
            Divider()
            Button("Refresh Library") { model.refreshLibrary(adoptOutput: true) }.keyboardShortcut("r")
        }
        CommandGroup(after: .toolbar) {
            Picker("View", selection: Bindable(model).viewMode) {
                Text("as Grid").tag(LibraryViewMode.grid)
                Text("as List").tag(LibraryViewMode.list)
            }
            Divider()
            Picker("Sort by", selection: Bindable(model).sort) {
                ForEach(LibrarySort.allCases) { Text(LocalizedStringKey($0.label)).tag($0) }
            }
        }
        CommandMenu("Device") {
            Button("Refresh Devices") { Task { await model.refreshIOS() } }.keyboardShortcut("r", modifiers: [.command, .option])
            Button("Install .ipa or .app…") { model.installFile() }.disabled(model.selectedDevice == nil)
        }
        CommandGroup(replacing: .help) {
            Button("Show Build Logs in Finder") { model.showLogsInFinder() }
            Button("Show Welcome Guide") { model.onboardingStep = 0; model.onboardingDone = false }
            Link("Napple Studio Guide", destination: URL(string: "https://napple.app/guides/studio")!)
        }
    }
}
