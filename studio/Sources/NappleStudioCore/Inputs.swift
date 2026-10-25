import Foundation

/// What `InputExpander.expand` made of the things the user dropped or picked.
public struct InputExpansion: Equatable, Sendable {
    /// Game files found (de-duplicated, stable order: dropped order, then name order inside folders).
    public var files: [URL] = []
    /// Dropped items that are not game files and hold none (a PDF, an empty folder, ...).
    public var ignored: [URL] = []
    /// Dropped items that do not exist (any more) or whose folder could not be read.
    public var unreadable: [URL] = []
    /// The folder walk stopped early (too many entries); some files may be missing.
    public var truncated = false
    public init() {}

    /// One sentence for the UI when something was skipped; nil when everything was usable.
    public var skippedNote: String? {
        var parts: [String] = []
        if !ignored.isEmpty { parts.append("\(ignored.count) \(ignored.count == 1 ? "item is" : "items are") not a game file") }
        if !unreadable.isEmpty { parts.append("\(unreadable.count) could not be read") }
        if truncated { parts.append("the folder is very large, so only the first \(InputExpander.maxEntries) entries were scanned") }
        return parts.isEmpty ? nil : "Skipped: " + parts.joined(separator: "; ") + "."
    }
}

/// Turns dropped files and folders into the list of game files `napple` understands (`.nsp .xci .nsz .xcz`, the same set as the CLI's own scan).
public enum InputExpander {
    public static let extensions: Set<String> = ["nsp", "xci", "nsz", "xcz"]
    public static let maxDepth = 6
    public static let maxEntries = 50_000

    public static func isGameFile(_ url: URL) -> Bool { extensions.contains(url.pathExtension.lowercased()) }

    public static func expand(_ urls: [URL]) -> InputExpansion {
        var out = InputExpansion()
        var seen = Set<String>()
        var visited = 0
        let fm = FileManager.default

        func add(_ u: URL) {
            let key = u.standardizedFileURL.resolvingSymlinksInPath().path
            if seen.insert(key).inserted { out.files.append(u.standardizedFileURL) }
        }
        func walk(_ dir: URL, depth: Int) {
            guard depth <= maxDepth, !out.truncated else { return }
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles]) else {
                out.unreadable.append(dir); return
            }
            for u in items.sorted(by: { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }) {
                visited += 1
                if visited > maxEntries { out.truncated = true; return }
                let isDir = (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
                if isDir { walk(u, depth: depth + 1) } else if isGameFile(u) { add(u) }
                if out.truncated { return }
            }
        }
        for u in urls {
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { out.unreadable.append(u); continue }
            if isDir.boolValue {
                let before = out.files.count
                walk(u, depth: 0)
                if out.files.count == before { out.ignored.append(u) }
            } else if isGameFile(u) {
                add(u)
            } else {
                out.ignored.append(u)
            }
        }
        return out
    }
}
