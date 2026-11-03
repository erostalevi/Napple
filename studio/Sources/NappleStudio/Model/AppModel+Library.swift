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
        // A game whose app is gone is simply not shown (it comes back by itself if the app returns), so changes need no message.
        _ = changes
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

    /// "Play with Debug": starts the game and opens a console window with its live log.
    func playWithDebug(_ item: LibraryItem) {
        guard let url = item.url, FileManager.default.fileExists(atPath: url.path) else { say(String(localized: "That app is gone.")); refreshLibrary(); return }
        if let running = consoles.first(where: { $0.app == url && $0.running }) { consoleToOpen = running.id; return }
        consoles.removeAll { !$0.running }
        let c = DebugConsole(name: item.entry.name, app: url)
        consoles.append(c)
        c.start()
        consoleToOpen = c.id
    }

    /// Games whose app exists. Missing ones are not shown.
    var presentLibrary: [LibraryItem] { libraryItems.filter { $0.status == .present } }

    var filteredLibrary: [LibraryItem] {
        let q = searchText.trimmingCharacters(in: .whitespaces)
        return q.isEmpty ? presentLibrary : presentLibrary.filter { $0.entry.name.localizedCaseInsensitiveContains(q) || $0.entry.titleId.localizedCaseInsensitiveContains(q) }
    }

    /// Cover art: the icon the build stored inside the app (from the game's own metadata).
    static func coverImage(for item: LibraryItem) -> NSImage? {
        guard item.status == .present else { return nil }
        let key = item.entry.path as NSString
        if let hit = coverCache.object(forKey: key) { return hit.image }
        let res = URL(fileURLWithPath: item.entry.path).appendingPathComponent("Contents/Resources")
        var found: NSImage?
        for n in ["AppIcon.icns", "icon.icns"] { if let i = NSImage(contentsOf: res.appendingPathComponent(n)) { found = i; break } }
        coverCache.setObject(CoverBox(found), forKey: key)
        return found
    }
    private static let coverCache = NSCache<NSString, CoverBox>()
    private final class CoverBox { let image: NSImage?; init(_ i: NSImage?) { image = i } }
}
