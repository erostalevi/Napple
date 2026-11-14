import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

enum LibraryViewMode: String, CaseIterable, Identifiable {
    case grid, list
    var id: String { rawValue }
    var label: LocalizedStringKey { self == .grid ? "Grid" : "List" }
    var symbol: String { self == .grid ? "square.grid.2x2" : "list.bullet" }
}

/// The build state a library tile can show.
enum LibraryBadge: Equatable {
    case none
    case building(Double)
    case compat(CompatStatus)
}

extension AppModel {
    // MARK: windows

    /// Opens the Settings window on a tab ("general", "engine", "keys", "devices", "controllers", "amiibo", "mii", "log").
    func openSettings(tab: String? = nil) {
        if let tab { settingsTab = tab }
        NSApp.activate()
        // the app menu's Settings item (Cmd-,) is the one reliable way to open the Settings scene from code
        if let menu = NSApp.mainMenu?.items.first?.submenu, let i = menu.items.firstIndex(where: { $0.keyEquivalent == "," }) { menu.performActionForItem(at: i) }
        else { NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil) }
    }

    func showKeysSetup() { openSettings(tab: "keys") }

    // MARK: selection and details

    func loadDetails() {
        guard let item = selectedItem, let url = item.url else { selectedDetails = nil; return }
        let id = item.id, tid = item.entry.titleId, support = keysets.root
        selectedDetails = nil
        Task.detached {
            let d = GameDetails.read(app: url, titleId: tid, support: support)
            await MainActor.run { if self.selectedLibraryItem == id { self.selectedDetails = d } }
        }
    }

    func badge(for item: LibraryItem) -> LibraryBadge {
        if let c = item.compat, c.status.needsAttention { return .compat(c.status) }
        return .none
    }

    /// Moves the selection by `delta` places in the shown order (arrow keys); with nothing selected the first game is picked.
    func moveSelection(by delta: Int) {
        let shown = filteredLibrary
        guard !shown.isEmpty else { return }
        guard let cur = selectedLibraryItem, let i = shown.firstIndex(where: { $0.id == cur }) else { selectedLibraryItem = shown.first?.id; return }
        selectedLibraryItem = shown[min(max(i + delta, 0), shown.count - 1)].id
    }

    // MARK: per-game actions

    func openSaves(_ item: LibraryItem) {
        let d = GameDetails.read(app: URL(fileURLWithPath: item.entry.path), titleId: item.entry.titleId, support: keysets.root)
        if let s = d.saveFolder { NSWorkspace.shared.open(s) }
        else if let f = d.dataFolder { NSWorkspace.shared.open(f) }
        else { say(String(localized: "No saves yet. They appear after the game has run once.")) }
    }

    func openGameData(_ item: LibraryItem) {
        let f = GameDetails.dataFolderPath(titleId: item.entry.titleId, support: keysets.root)
        if FileManager.default.fileExists(atPath: f.path) { NSWorkspace.shared.open(f) }
        else { say(String(localized: "Nothing to show yet. The folder appears after the game has run once.")) }
    }

    /// Update or DLC: the game's files are chosen again together with the new files, and the app is rebuilt in place.
    func rebuildWithNewFiles(_ item: LibraryItem) {
        guard !sets.isEmpty else { showKeysSetup(); return }
        let urls = pick(files: true, dirs: true, multiple: true,
                        message: String(localized: "Choose the base game and the new update or DLC for \(item.entry.name)"))
        if !urls.isEmpty { Task { await handleDrop(urls) } }
    }

    /// Mods: choose .napmod files or mod folders, then the game's files; the app is rebuilt with them.
    func rebuildWithMods(_ item: LibraryItem) {
        guard !sets.isEmpty else { showKeysSetup(); return }
        let mods = chooseMods()
        guard !mods.isEmpty else { return }
        let urls = pick(files: true, dirs: true, multiple: true,
                        message: String(localized: "Now choose the game's files for \(item.entry.name). It is rebuilt with the mods."))
        guard !urls.isEmpty else { pickedMods = []; return }
        Task { await handleDrop(urls); pickedMods = mods }
    }

    /// Adds mods to the build in the plan panel.
    func addMods() {
        let r = ModPicker.filter(chooseMods(), existing: pickedMods)
        pickedMods += r.accepted
    }

    private func chooseMods() -> [URL] {
        let picked = pick(files: true, dirs: true, multiple: true, message: String(localized: "Choose mods (.napmod files or mod folders)"))
        let r = ModPicker.filter(picked, existing: pickedMods)
        if !r.rejected.isEmpty { say(String(localized: "\(r.rejected.count) of the chosen items are not mods and were skipped. A mod is a .napmod file or a folder with a mod.json.")) }
        return r.accepted
    }

    func removeMod(_ url: URL) { pickedMods.removeAll { $0 == url } }

    // MARK: engine

    /// "Napple engine 0.1.0 (12)" or nil when none is found.
    var engineSummary: String? {
        guard cli != nil else { return nil }
        if let e = selectedEngine { return e.label }
        return String(localized: "Bundled engine")
    }
}

extension AppModel {
    /// "0.1.0 Beta (123)" from the bundle (the debug build has none: it shows 0.1.0 Beta).
    static var versionString: String {
        let info = Bundle.main.infoDictionary
        return AppVersion.display(version: info?["CFBundleShortVersionString"] as? String, build: info?["CFBundleVersion"] as? String)
    }
}
