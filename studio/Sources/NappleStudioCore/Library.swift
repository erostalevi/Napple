import Foundation

/// What Studio reads back out of a built Napple app's Info.plist (see `crates/napple-build/src/bundle.rs`).
public struct BuiltAppInfo: Equatable, Sendable {
    public var name: String
    public var bundleId: String
    public var version: String
    public var titleId: String      // 16 hex digits, upper case
    public var isMac: Bool          // macOS bundles keep Info.plist in Contents/, iOS bundles are flat

    public init(name: String, bundleId: String, version: String, titleId: String, isMac: Bool) {
        self.name = name; self.bundleId = bundleId; self.version = version; self.titleId = titleId; self.isMac = isMac
    }

    /// nil when `app` is not a Napple app (no `NappleTitleId`, and no `com.napple.game.<titleid>` bundle id either).
    public static func read(_ app: URL) -> BuiltAppInfo? {
        let fm = FileManager.default
        let macPlist = app.appendingPathComponent("Contents/Info.plist"), flatPlist = app.appendingPathComponent("Info.plist")
        let isMac = fm.fileExists(atPath: macPlist.path)
        guard let d = NSDictionary(contentsOf: isMac ? macPlist : flatPlist) as? [String: Any] else { return nil }
        let bundleId = d["CFBundleIdentifier"] as? String ?? ""
        var titleId = (d["NappleTitleId"] as? String)?.uppercased()
        if titleId == nil, bundleId.hasPrefix("com.napple.game.") {
            let tail = String(bundleId.dropFirst("com.napple.game.".count))
            if tail.count == 16, tail.allSatisfy(\.isHexDigit) { titleId = tail.uppercased() }
        }
        guard let tid = titleId, !tid.isEmpty else { return nil }
        let name = (d["CFBundleDisplayName"] as? String) ?? (d["CFBundleName"] as? String) ?? app.deletingPathExtension().lastPathComponent
        return BuiltAppInfo(name: name, bundleId: bundleId, version: d["CFBundleShortVersionString"] as? String ?? "",
                            titleId: tid, isMac: isMac)
    }

    /// Bytes the bundle takes on disk (APFS clones count in full; good enough for a library card).
    public static func sizeOnDisk(_ url: URL) -> UInt64 {
        let keys: [URLResourceKey] = [.totalFileAllocatedSizeKey, .fileAllocatedSizeKey]
        guard let e = FileManager.default.enumerator(at: url, includingPropertiesForKeys: keys) else { return 0 }
        var total: UInt64 = 0
        for case let f as URL in e {
            let v = try? f.resourceValues(forKeys: Set(keys))
            total += UInt64(v?.totalFileAllocatedSize ?? v?.fileAllocatedSize ?? 0)
        }
        return total
    }
}

/// One built macOS app Studio keeps in its library.
public struct LibraryEntry: Codable, Identifiable, Equatable, Sendable {
    public var id: UUID
    public var titleId: String
    public var name: String
    public var version: String
    public var builtAt: Date
    public var bytes: UInt64?
    /// Last known place of the .app.
    public var path: String
    /// Finder-style bookmark: finds the app again after it was moved or renamed.
    public var bookmark: Data?

    public init(id: UUID = UUID(), titleId: String, name: String, version: String, builtAt: Date, bytes: UInt64?, path: String, bookmark: Data?) {
        self.id = id; self.titleId = titleId; self.name = name; self.version = version
        self.builtAt = builtAt; self.bytes = bytes; self.path = path; self.bookmark = bookmark
    }
}

public enum LibraryStatus: Equatable, Sendable {
    /// The app is where the library says (or was followed to its new place).
    case present
    /// Nothing there any more (deleted, on an unmounted volume, or being rebuilt right now).
    case missing
    /// Something else lives at the old path and the bookmark finds nothing: it is not this game's app.
    case replaced
}

public struct LibraryChange: Equatable, Sendable {
    public enum Kind: Equatable, Sendable { case moved, missing, restored, trashed }
    public var kind: Kind
    public var name: String
    public var path: String?
    public init(kind: Kind, name: String, path: String? = nil) { self.kind = kind; self.name = name; self.path = path }
}

public enum LibraryError: Error, LocalizedError, Equatable {
    case notNappleApp
    case notMacApp
    case differentGame(String)
    public var errorDescription: String? {
        switch self {
        case .notNappleApp: return "That is not an app built by Napple."
        case .notMacApp: return "Only Mac apps can be kept in the library."
        case .differentGame(let n): return "That app is not \(n)."
        }
    }
}

/// The list of games Studio has built, kept in `<support>/library.json`.
///
/// Only macOS apps are listed. Each entry follows its .app: the path is checked on every `refresh()`, and when it is gone the stored
/// bookmark finds the app again after a move or rename in Finder. An app moved to the Trash leaves the library; an app that
/// simply vanishes stays, flagged `missing`, so the user can locate it again or remove it.
public final class LibraryStore: @unchecked Sendable {
    public let file: URL
    private let lock = NSLock()
    private var items: [LibraryEntry] = []
    private var statuses: [UUID: LibraryStatus] = [:]

    public init(file: URL) {
        self.file = file
        load()
    }

    // MARK: reading

    /// Newest build first.
    public var entries: [LibraryEntry] {
        lock.lock(); defer { lock.unlock() }
        return items.sorted { $0.builtAt > $1.builtAt }
    }

    public func status(of id: UUID) -> LibraryStatus {
        lock.lock(); defer { lock.unlock() }
        return statuses[id] ?? .present
    }

    public func entry(_ id: UUID) -> LibraryEntry? {
        lock.lock(); defer { lock.unlock() }
        return items.first { $0.id == id }
    }

    /// The .app to open, when it is there.
    public func url(of id: UUID) -> URL? {
        guard let e = entry(id), status(of: id) == .present else { return nil }
        return URL(fileURLWithPath: e.path)
    }

    // MARK: changing

    /// Adds a freshly built app, or refreshes the entry already pointing at the same path (a rebuild into the same folder).
    @discardableResult
    public func add(app: URL, info: BuiltAppInfo, bytes: UInt64?, builtAt: Date = Date()) throws -> LibraryEntry {
        guard info.isMac else { throw LibraryError.notMacApp }
        let path = app.standardizedFileURL.path
        let bookmark = Self.bookmark(for: app)
        lock.lock(); defer { lock.unlock() }
        if let i = items.firstIndex(where: { $0.path == path || ($0.titleId == info.titleId && Self.samePlace($0, as: app)) }) {
            items[i].titleId = info.titleId; items[i].name = info.name; items[i].version = info.version
            items[i].builtAt = builtAt; items[i].bytes = bytes ?? items[i].bytes; items[i].path = path; items[i].bookmark = bookmark
            statuses[items[i].id] = .present
            save()
            return items[i]
        }
        let e = LibraryEntry(titleId: info.titleId, name: info.name, version: info.version, builtAt: builtAt, bytes: bytes, path: path, bookmark: bookmark)
        items.append(e)
        statuses[e.id] = .present
        save()
        return e
    }

    public func remove(_ id: UUID) {
        lock.lock(); defer { lock.unlock() }
        items.removeAll { $0.id == id }
        statuses[id] = nil
        save()
    }

    /// "Locate...": points a missing entry at an app the user picked. It has to be the same game.
    @discardableResult
    public func relocate(_ id: UUID, to app: URL) throws -> LibraryEntry {
        guard let info = BuiltAppInfo.read(app) else { throw LibraryError.notNappleApp }
        guard info.isMac else { throw LibraryError.notMacApp }
        lock.lock(); defer { lock.unlock() }
        guard let i = items.firstIndex(where: { $0.id == id }) else { throw LibraryError.notNappleApp }
        guard info.titleId == items[i].titleId.uppercased() else { throw LibraryError.differentGame(items[i].name) }
        items[i].path = app.standardizedFileURL.path
        items[i].bookmark = Self.bookmark(for: app)
        items[i].name = info.name; items[i].version = info.version
        statuses[id] = .present
        save()
        return items[i]
    }

    /// Checks every entry against the disk and follows apps that moved. Returns what changed (for a toast).
    @discardableResult
    public func refresh() -> [LibraryChange] {
        lock.lock(); defer { lock.unlock() }
        var changes: [LibraryChange] = []
        var dirty = false
        var drop: [UUID] = []
        for i in items.indices {
            let e = items[i]
            let before = statuses[e.id] ?? .present
            let storedURL = URL(fileURLWithPath: e.path)
            var after = LibraryStatus.missing

            if let found = Self.validApp(at: storedURL, titleId: e.titleId) {
                after = .present
                if found.name != e.name || found.version != e.version { items[i].name = found.name; items[i].version = found.version; dirty = true }
                if e.bookmark == nil, let b = Self.bookmark(for: storedURL) { items[i].bookmark = b; dirty = true }
            } else if let (moved, stale) = Self.resolve(e.bookmark) {
                if Self.isInTrash(moved) {
                    changes.append(LibraryChange(kind: .trashed, name: e.name, path: moved.path))
                    drop.append(e.id); continue
                }
                if let found = Self.validApp(at: moved, titleId: e.titleId) {
                    after = .present
                    items[i].path = moved.standardizedFileURL.path
                    items[i].name = found.name; items[i].version = found.version
                    if stale || e.bookmark == nil, let b = Self.bookmark(for: moved) { items[i].bookmark = b }
                    dirty = true
                    changes.append(LibraryChange(kind: .moved, name: found.name, path: moved.path))
                }
            }
            if after == .missing, FileManager.default.fileExists(atPath: e.path) { after = .replaced }

            statuses[e.id] = after
            if after == .present, before != .present, !changes.contains(where: { $0.name == e.name && $0.kind == .moved }) {
                if before == .missing || before == .replaced { changes.append(LibraryChange(kind: .restored, name: e.name, path: items[i].path)) }
            } else if after != .present, before == .present {
                changes.append(LibraryChange(kind: .missing, name: e.name, path: e.path))
            }
        }
        if !drop.isEmpty {
            items.removeAll { drop.contains($0.id) }
            for id in drop { statuses[id] = nil }
            dirty = true
        }
        if dirty { save() }
        return changes
    }

    /// Picks up Napple Mac apps sitting directly in `directory` that the library does not know yet (built by the CLI, or by an older Studio).
    @discardableResult
    public func adopt(directory: URL) -> Int {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return 0 }
        var added = 0
        for n in names.sorted() where n.hasSuffix(".app") {
            let app = directory.appendingPathComponent(n, isDirectory: true)
            guard let info = BuiltAppInfo.read(app), info.isMac else { continue }
            let path = app.standardizedFileURL.path
            lock.lock()
            let known = items.contains { $0.path == path }
            lock.unlock()
            if known { continue }
            let built = (try? app.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date()
            if (try? add(app: app, info: info, bytes: nil, builtAt: built)) != nil { added += 1 }
        }
        return added
    }

    /// Folders that hold library apps; the watcher listens to these.
    public var watchedDirectories: Set<String> {
        lock.lock(); defer { lock.unlock() }
        return Set(items.map { URL(fileURLWithPath: $0.path).deletingLastPathComponent().path })
    }

    // MARK: persistence

    private struct Disk: Codable { var v: Int; var entries: [LibraryEntry] }

    private func load() {
        guard let data = try? Data(contentsOf: file) else { return }
        let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
        if let d = try? dec.decode(Disk.self, from: data) {
            items = d.entries
        } else {
            // unreadable file: keep a copy for inspection and start empty rather than failing every launch
            try? FileManager.default.moveItem(at: file, to: file.appendingPathExtension("bad"))
        }
    }

    /// Caller holds the lock.
    private func save() {
        let enc = JSONEncoder(); enc.dateEncodingStrategy = .iso8601; enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? enc.encode(Disk(v: 1, entries: items)) else { return }
        try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let tmp = file.appendingPathExtension("tmp")
        do {
            try data.write(to: tmp, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: tmp.path)
            _ = try FileManager.default.replaceItemAt(file, withItemAt: tmp)
        } catch {
            try? data.write(to: file, options: .atomic)
        }
    }

    // MARK: helpers

    private static func validApp(at url: URL, titleId: String) -> BuiltAppInfo? {
        guard FileManager.default.fileExists(atPath: url.path), let info = BuiltAppInfo.read(url), info.isMac,
              info.titleId == titleId.uppercased() else { return nil }
        return info
    }

    private static func bookmark(for url: URL) -> Data? {
        try? url.bookmarkData(options: [.minimalBookmark], includingResourceValuesForKeys: nil, relativeTo: nil)
    }

    private static func resolve(_ data: Data?) -> (URL, Bool)? {
        guard let data else { return nil }
        var stale = false
        guard let u = try? URL(resolvingBookmarkData: data, options: [.withoutUI, .withoutMounting], relativeTo: nil, bookmarkDataIsStale: &stale) else { return nil }
        return (u, stale)
    }

    private static func isInTrash(_ url: URL) -> Bool { url.path.contains("/.Trash/") }

    private static func samePlace(_ e: LibraryEntry, as app: URL) -> Bool {
        guard let (u, _) = resolve(e.bookmark) else { return false }
        return u.standardizedFileURL.path == app.standardizedFileURL.path
    }
}

/// Calls `onChange` (debounced) whenever something is added to, removed from or renamed in one of the watched folders.
public final class DirectoryWatcher: @unchecked Sendable {
    private let queue = DispatchQueue(label: "app.napple.studio.watcher")
    private var sources: [String: DispatchSourceFileSystemObject] = [:]
    private var pending: DispatchWorkItem?
    private let onChange: @Sendable () -> Void

    public init(onChange: @escaping @Sendable () -> Void) { self.onChange = onChange }

    /// Starts watching new folders and stops watching ones no longer listed. Folders that do not exist are skipped.
    public func watch(_ paths: Set<String>) {
        queue.async { [self] in
            for (p, s) in sources where !paths.contains(p) { s.cancel(); sources[p] = nil }
            for p in paths where sources[p] == nil {
                let fd = open(p, O_EVTONLY)
                if fd < 0 { continue }
                let s = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete, .link, .attrib], queue: queue)
                s.setEventHandler { [weak self, weak s] in
                    guard let self else { return }
                    if let s, !s.data.intersection([.rename, .delete]).isEmpty {
                        // the watched folder itself went away: let the next watch() call re-open it
                        s.cancel(); self.sources[p] = nil
                    }
                    self.fire()
                }
                s.setCancelHandler { close(fd) }
                sources[p] = s
                s.resume()
            }
        }
    }

    public func stop() {
        queue.async { [self] in
            for (_, s) in sources { s.cancel() }
            sources = [:]
            pending?.cancel()
        }
    }

    private func fire() {
        pending?.cancel()
        let w = DispatchWorkItem { [onChange] in onChange() }
        pending = w
        queue.asyncAfter(deadline: .now() + 0.4, execute: w)
    }
}
