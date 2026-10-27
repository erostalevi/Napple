import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

extension AppModel {
    func refreshLibrary(adoptOutput: Bool = false) {
        if adoptOutput { library.adopt(directory: outputDir) }
        let changes = library.refresh()
        reloadItems()
        syncWatcher()
        guard libraryLoaded else { return }
        for c in changes {
            switch c.kind {
            case .trashed: say(String(localized: "\(c.name) was moved to the Trash, so it left your library."))
            case .moved: say(String(localized: "Found \(c.name) at its new place."))
            case .missing: say(String(localized: "\(c.name) can't be found any more. Locate it or remove it from the library."))
            case .restored: break
            }
        }
    }

    func reloadItems() {
        libraryItems = library.entries.map { LibraryItem(entry: $0, status: library.status(of: $0.id), compat: matrix?.entry(for: $0.titleId)) }
    }

    func syncWatcher() {
        var dirs = library.watchedDirectories
        if FileManager.default.fileExists(atPath: outputDir.path) { dirs.insert(outputDir.path) }
        watcher?.watch(dirs)
    }

    func registerInLibrary(_ app: URL) async {
        guard let info = BuiltAppInfo.read(app), info.isMac else { return }
        let bytes = await Task.detached { BuiltAppInfo.sizeOnDisk(app) }.value
        builtBytes = bytes
        do { try library.add(app: app, info: info, bytes: bytes) }
        catch { installMessage = String(localized: "Built, but it could not be added to the library: \(error.localizedDescription)") }
        reloadItems(); syncWatcher()
    }

    /// Opens a game (amiibo and the IR camera are chosen in the game's own overlay, F1).
    func launch(_ url: URL?) {
        guard let url else { return }
        guard FileManager.default.fileExists(atPath: url.path) else { say(String(localized: "That app is gone. Refreshing your library.")); refreshLibrary(); return }
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration()) { _, error in
            if let error { DispatchQueue.main.async { self.say(String(localized: "Could not open the app: \(error.localizedDescription)")) } }
        }
    }

    func reveal(_ url: URL?) {
        guard let url, FileManager.default.fileExists(atPath: url.path) else { say(String(localized: "That app is gone. Refreshing your library.")); refreshLibrary(); return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    func installToApplications(_ a: URL) {
        let dest = URL(fileURLWithPath: "/Applications").appendingPathComponent(a.lastPathComponent)
        if dest.standardizedFileURL == a.standardizedFileURL { say(String(localized: "It is already in /Applications.")); return }
        do {
            if FileManager.default.fileExists(atPath: dest.path) { try FileManager.default.trashItem(at: dest, resultingItemURL: nil) }
            try FileManager.default.copyItem(at: a, to: dest)
            say(String(localized: "Installed to /Applications."))
        } catch { say(String(localized: "Install failed: \(error.localizedDescription) You can drag the app into Applications yourself.")) }
    }

    func removeFromLibrary(_ id: UUID) { library.remove(id); if selectedLibraryItem == id { selectedLibraryItem = nil }; reloadItems(); syncWatcher() }

    func trashFromLibrary(_ item: LibraryItem) {
        guard let url = item.url else { removeFromLibrary(item.id); return }
        do {
            try FileManager.default.trashItem(at: url, resultingItemURL: nil)
            removeFromLibrary(item.id)
            say(String(localized: "Moved \(item.entry.name) to the Trash."))
        } catch { say(String(localized: "Could not move it to the Trash: \(error.localizedDescription)")) }
    }

    func locate(_ item: LibraryItem) {
        guard let app = pick(files: true, dirs: false, message: String(localized: "Choose \(item.entry.name).app"), types: [.application]).first else { return }
        do { try library.relocate(item.id, to: app); reloadItems(); syncWatcher(); say(String(localized: "Found \(item.entry.name).")) }
        catch { say(error.localizedDescription) }
    }

    var filteredLibrary: [LibraryItem] {
        let q = searchText.trimmingCharacters(in: .whitespaces)
        return q.isEmpty ? libraryItems : libraryItems.filter { $0.entry.name.localizedCaseInsensitiveContains(q) || $0.entry.titleId.localizedCaseInsensitiveContains(q) }
    }

    /// Cover art: the icon the build stored inside the app (from the game's own metadata).
    static func coverImage(for item: LibraryItem) -> NSImage? {
        guard item.status == .present else { return nil }
        let res = URL(fileURLWithPath: item.entry.path).appendingPathComponent("Contents/Resources")
        for n in ["AppIcon.icns", "icon.icns"] { if let i = NSImage(contentsOf: res.appendingPathComponent(n)) { return i } }
        return nil
    }
}
