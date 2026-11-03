import SwiftUI
import AppKit

/// Content of the `Settings { }` scene: `Settings { SettingsRoot().environment(model) }`.
/// Eight icon tabs in the toolbar, a fixed-size translucent window. The selected tab lives in `AppModel.settingsTab`
/// ("general", "keys", "devices", "mii", "amiibo", "online", "engine", "log"), so other code can open Settings on a given tab.
struct SettingsRoot: View {
    @Environment(AppModel.self) private var m
    static let size = CGSize(width: 720, height: 600)

    var body: some View {
        @Bindable var m = m
        TabView(selection: $m.settingsTab) {
            Tab("General", systemImage: "gearshape", value: "general") { GeneralSettingsTab() }
            Tab("Keys", systemImage: "key", value: "keys") { KeysSettingsTab() }
            Tab("iPhone & iPad", systemImage: "iphone.gen3", value: "devices") { DevicesSettingsTab() }
            Tab("Mii", systemImage: "person.crop.square", value: "mii") { MiiSettingsTab() }
            Tab("Amiibo", systemImage: "wave.3.right", value: "amiibo") { AmiiboSettingsTab() }
            Tab("Online", systemImage: "network", value: "online") { OnlineSettingsTab() }
            Tab("Engine", systemImage: "cpu", value: "engine") { EngineSettingsTab() }
            Tab("Log", systemImage: "text.alignleft", value: "log") { LogSettingsTab() }
        }
        .frame(width: Self.size.width, height: Self.size.height)
        .containerBackground(.ultraThinMaterial, for: .window)
        .onAppear { if let t = SettingsDebug.tab { m.settingsTab = t } }
    }
}

/// `--debug-settings <tab>` selects a tab when Settings opens; `SettingsDebug.openIfRequested()` opens the window at launch (for screenshots).
enum SettingsDebug {
    static var tab: String? {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "--debug-settings"), i + 1 < a.count else { return nil }
        return a[i + 1]
    }

    @MainActor
    static func openIfRequested() {
        guard tab != nil else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            NSApp.activate()
            // the app menu's Settings item (Cmd-,) is the one reliable way to open the Settings scene from code
            if let menu = NSApp.mainMenu?.items.first?.submenu, let i = menu.items.firstIndex(where: { $0.keyEquivalent == "," }) { menu.performActionForItem(at: i) }
        }
    }
}
