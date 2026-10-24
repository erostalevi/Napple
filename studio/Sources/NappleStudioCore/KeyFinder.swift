import Foundation

/// What `KeyFinder.scan` found in a folder. Paths only; key values are never read beyond key NAMES (for validation).
public struct KeyScan: Equatable, Sendable {
    public var keys: URL?
    public var titleKeys: URL?
    public var firmware: URL?
    public init(keys: URL? = nil, titleKeys: URL? = nil, firmware: URL? = nil) { self.keys = keys; self.titleKeys = titleKeys; self.firmware = firmware }
}

/// Onboarding "choose a folder": finds prod.keys, title.keys and the firmware under one folder by their usual names.
public enum KeyFinder {
    /// Key files by preference: `prod.keys`, then the older `keys.txt` / `keys.ini` names (only files that pass the required-key check).
    static let keyNames = ["prod.keys", "keys.txt", "keys.ini"]
    static let titleKeyNames = ["title.keys", "titlekeys.txt"]
    static let maxDepth = 3, maxEntries = 20_000

    /// Searches `folder` and its subfolders (3 levels, hidden items skipped). Firmware: a folder of .nca files (the chosen folder itself,
    /// or one named like "firmware"), or a .zip named like "firmware" or a version ("19.0.1.zip"); with several, the highest version in the name wins.
    public static func scan(_ folder: URL) -> KeyScan {
        let fm = FileManager.default
        var files: [URL] = [], dirs: [URL] = [folder]
        var queue: [(URL, Int)] = [(folder, 0)], seen = 0
        while !queue.isEmpty, seen < maxEntries {
            let (dir, depth) = queue.removeFirst()
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles]) else { continue }
            for u in items.sorted(by: { $0.path < $1.path }) {
                seen += 1
                if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                    dirs.append(u)
                    if depth + 1 < maxDepth { queue.append((u, depth + 1)) }
                } else { files.append(u) }
            }
        }
        var r = KeyScan()
        func named(_ names: [String]) -> [URL] {
            names.flatMap { n in files.filter { $0.lastPathComponent.lowercased() == n }.sorted { depth($0, folder) < depth($1, folder) } }
        }
        r.keys = named(keyNames).first { (try? KeysetManager.missingKeyNames(in: $0).isEmpty) == true }
        r.titleKeys = named(titleKeyNames).first
        // firmware candidates: folders holding .nca files directly, and firmware-named zips
        let ncaDirs = dirs.filter { d in
            (d == folder || d.lastPathComponent.lowercased().contains("firmware") || d.deletingLastPathComponent().lastPathComponent.lowercased().contains("firmware"))
                && ((try? fm.contentsOfDirectory(atPath: d.path))?.contains { $0.lowercased().hasSuffix(".nca") } ?? false)
        }
        let zips = files.filter { $0.pathExtension.lowercased() == "zip" && ($0.lastPathComponent.lowercased().contains("firmware") || version($0).count >= 2) }
        r.firmware = (ncaDirs + zips).sorted { version($1).lexicographicallyPrecedes(version($0)) }.first { KeysetManager.isValidFirmware($0) }
        return r
    }

    static func depth(_ u: URL, _ root: URL) -> Int { u.pathComponents.count - root.pathComponents.count }

    /// "Firmware 19.0.1.zip" -> [19, 0, 1] (compared lexicographically); [] when the name has no version.
    static func version(_ u: URL) -> [Int] {
        let name = u.deletingPathExtension().lastPathComponent
        guard let r = name.range(of: #"\d+(\.\d+)+"#, options: .regularExpression) else { return [] }
        return name[r].split(separator: ".").compactMap { Int($0) }
    }
}
