import SwiftUI
import AppKit
import NappleStudioCore

/// Model support for the Settings window. UserDefaults keys used here (documented for the main window):
///  - `playAfterBuild` (Bool, default false): "Play the game when the build finishes (Mac)", read with @AppStorage by both windows.
///  - `activeKeySet` (String): name of the key set in use.
extension AppModel {
    static let playAfterBuildKey = "playAfterBuild"
    static let activeKeySetKey = "activeKeySet"

    // MARK: key sets: the set created last is the one in use

    /// Re-reads the sets from disk and applies the remembered choice (nothing remembered: the newest set; remembered but gone: the first one).
    /// Call it after `refreshSets()` at launch so builds use the same set Settings shows as "In use".
    func restoreActiveSet() {
        sets = keysets.list()
        selectedSetName = ActiveKeyset.resolve(remembered: UserDefaults.standard.string(forKey: Self.activeKeySetKey), in: sets)
        UserDefaults.standard.set(selectedSetName, forKey: Self.activeKeySetKey)
    }

    /// After a set was created: it becomes the active one.
    func keySetCreated(_ set: Keyset) {
        sets = keysets.list()
        selectedSetName = set.name
        UserDefaults.standard.set(set.name, forKey: Self.activeKeySetKey)
        keysChanged()
    }

    /// Deletes a set from disk (off the main thread: firmware folders are large). When it was the active one, the first remaining set becomes active.
    func deleteKeySet(_ set: Keyset) async throws {
        let mgr = keysets
        try await Task.detached { try mgr.delete(set) }.value
        let wasActive = selectedSetName == set.name
        sets = keysets.list()
        if wasActive || !sets.contains(where: { $0.name == selectedSetName }) {
            selectedSetName = sets.first?.name ?? ""
            UserDefaults.standard.set(selectedSetName, forKey: Self.activeKeySetKey)
        }
        keysChanged()
    }

    // MARK: engines

    /// Where installed engine versions live for this Studio's support folder (honours `--support-dir`).
    var engineStore: EngineStore {
        EngineStore(engineRoot: EngineInstaller(resources: Bundle.main.resourceURL ?? keysets.root, support: keysets.root).engineRoot, tools: tools)
    }

    /// The engine the radio list shows as selected: the picked one, else the one the default lookup is most likely using.
    var effectiveEngineID: String { selectedEngine?.id ?? engines.first(where: \.isActive)?.id ?? engines.first?.id ?? "" }

    // MARK: log

    /// The newest `build-*.log` on disk.
    nonisolated static func newestLogFile(in dir: URL) -> URL? {
        let items = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        func stamp(_ u: URL) -> Date { (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }
        return items.filter { $0.lastPathComponent.hasPrefix("build-") && $0.pathExtension == "log" }.max { stamp($0) < stamp($1) }
    }

    /// The last `maxLines` lines of a log file, reading only its tail.
    nonisolated static func tail(of file: URL, maxLines: Int = 2000, maxBytes: Int = 1_000_000) -> [String] {
        guard let h = try? FileHandle(forReadingFrom: file) else { return [] }
        defer { try? h.close() }
        guard let size = try? h.seekToEnd() else { return [] }
        let start = size > UInt64(maxBytes) ? size - UInt64(maxBytes) : 0
        try? h.seek(toOffset: start)
        guard let data = try? h.readToEnd() else { return [] }
        var lines = String(decoding: data, as: UTF8.self).split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        if start > 0, !lines.isEmpty { lines.removeFirst() }   // the first line is probably cut
        if lines.last == "" { lines.removeLast() }
        return Array(lines.suffix(maxLines))
    }
}
