import Foundation

// MARK: - sorting and searching the library

/// How the library grid and list are ordered.
public enum LibrarySort: String, CaseIterable, Identifiable, Sendable {
    case recent, name, size, titleId
    public var id: String { rawValue }
    public var label: String {
        switch self {
        case .recent: "Recently Built"
        case .name: "Name"
        case .size: "Size"
        case .titleId: "Title ID"
        }
    }
    public var symbol: String {
        switch self {
        case .recent: "clock"
        case .name: "textformat"
        case .size: "externaldrive"
        case .titleId: "number"
        }
    }

    /// Stable: equal keys fall back to the name, then to the title id, so the grid never shuffles between refreshes.
    public func sorted(_ entries: [LibraryEntry]) -> [LibraryEntry] {
        entries.sorted { a, b in
            switch self {
            case .recent: if a.builtAt != b.builtAt { return a.builtAt > b.builtAt }
            case .name: break
            case .size: if (a.bytes ?? 0) != (b.bytes ?? 0) { return (a.bytes ?? 0) > (b.bytes ?? 0) }
            case .titleId: if a.titleId != b.titleId { return a.titleId < b.titleId }
            }
            let c = a.name.localizedStandardCompare(b.name)
            if c != .orderedSame { return c == .orderedAscending }
            return a.titleId < b.titleId
        }
    }
}

extension LibraryEntry {
    /// Search: every word of the query must be in the name or the title id (case and diacritics ignored).
    public func matches(query: String) -> Bool {
        let words = query.split(whereSeparator: \.isWhitespace)
        if words.isEmpty { return true }
        return words.allSatisfy { w in
            name.range(of: w, options: [.caseInsensitive, .diacriticInsensitive]) != nil
                || titleId.range(of: w, options: .caseInsensitive) != nil
        }
    }
}

// MARK: - per-game folders and details

/// A mod baked into a built game (read back from the game's manifest).
public struct AppliedMod: Equatable, Sendable, Identifiable {
    public var id: String
    public var version: String
    public init(id: String, version: String) { self.id = id; self.version = version }
}

/// What the game's detail view shows, read from the built app and the support folder (no CLI call).
public struct GameDetails: Equatable, Sendable {
    /// Game versions embedded in the app (`Contents/Resources/game/<version>/`), the default one first.
    public var embeddedVersions: [String] = []
    public var mods: [AppliedMod] = []
    /// `<support>/<TITLEID>`: the game's settings and saves. Nil until the game has run once.
    public var dataFolder: URL?
    /// `<support>/<TITLEID>/save`, when the game has written saves.
    public var saveFolder: URL?
    public var saveBytes: UInt64 = 0
    public var hasCustomSettings = false

    public init() {}

    public static func dataFolderPath(titleId: String, support: URL) -> URL {
        support.appendingPathComponent(titleId.uppercased(), isDirectory: true)
    }

    public static func read(app: URL, titleId: String, support: URL = SystemData.supportDir()) -> GameDetails {
        let fm = FileManager.default
        var d = GameDetails()
        let game = app.appendingPathComponent("Contents/Resources/game", isDirectory: true)
        let def = (try? String(contentsOf: game.appendingPathComponent("default"), encoding: .utf8))?
            .split(whereSeparator: \.isNewline).first.map(String.init)
        let dirs = ((try? fm.contentsOfDirectory(atPath: game.path)) ?? []).filter { n in
            var isDir: ObjCBool = false
            return fm.fileExists(atPath: game.appendingPathComponent(n).path, isDirectory: &isDir) && isDir.boolValue
        }.sorted()
        d.embeddedVersions = dirs.sorted { a, b in (a == def) != (b == def) ? a == def : a < b }
        if let v = def ?? dirs.first, let m = manifest(game.appendingPathComponent(v)) { d.mods = m }

        let data = dataFolderPath(titleId: titleId, support: support)
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: data.path, isDirectory: &isDir), isDir.boolValue {
            d.dataFolder = data
            d.hasCustomSettings = fm.fileExists(atPath: data.appendingPathComponent("settings.json").path)
            let save = data.appendingPathComponent("save", isDirectory: true)
            if fm.fileExists(atPath: save.path, isDirectory: &isDir), isDir.boolValue {
                d.saveFolder = save
                d.saveBytes = BuiltAppInfo.sizeOnDisk(save)
            }
        }
        return d
    }

    private static func manifest(_ versionDir: URL) -> [AppliedMod]? {
        guard let data = try? Data(contentsOf: versionDir.appendingPathComponent("manifest.json")),
              let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let mods = o["mods"] as? [[String: Any]] else { return nil }
        return mods.compactMap { m in
            guard let id = m["id"] as? String, !id.isEmpty else { return nil }
            return AppliedMod(id: id, version: m["version"] as? String ?? "")
        }
    }
}

// MARK: - mods picked for a build

public enum ModPicker {
    /// A `.napmod` file, or a folder that holds a `mod.json`.
    public static func isMod(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { return false }
        if isDir.boolValue { return FileManager.default.fileExists(atPath: url.appendingPathComponent("mod.json").path) }
        return url.pathExtension.lowercased() == "napmod"
    }

    /// Keeps the mods in the order given, drops duplicates and anything that is not a mod; `rejected` is what was dropped (not duplicates).
    public static func filter(_ urls: [URL], existing: [URL] = []) -> (accepted: [URL], rejected: [URL]) {
        var seen = Set(existing.map { $0.standardizedFileURL.path })
        var ok: [URL] = [], bad: [URL] = []
        for u in urls {
            let p = u.standardizedFileURL.path
            guard isMod(u) else { bad.append(u); continue }
            if seen.insert(p).inserted { ok.append(u) }
        }
        return (ok, bad)
    }
}

// MARK: - version string

public enum AppVersion {
    /// "0.1.0 Beta (123)" for the About window; the build number is left out when it is empty.
    public static func display(version: String?, build: String?, beta: Bool = true) -> String {
        var s = (version?.isEmpty == false ? version! : "0.1.0")
        if beta { s += " Beta" }
        if let b = build, !b.isEmpty { s += " (\(b))" }
        return s
    }
}
