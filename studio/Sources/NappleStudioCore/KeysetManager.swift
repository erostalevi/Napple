import Foundation

/// A stored key/firmware set under `<root>/keysets/<name>/`.
public struct Keyset: Identifiable, Equatable, Sendable {
    public let name: String
    public let directory: URL
    public var id: String { name }
    public var keysURL: URL { directory.appendingPathComponent("prod.keys") }
    public var titleKeysURL: URL? {
        let u = directory.appendingPathComponent("title.keys")
        return FileManager.default.fileExists(atPath: u.path) ? u : nil
    }
    /// Folder `firmware/` or file `firmware.zip`, when present.
    public var firmwareURL: URL? {
        for n in ["firmware", "firmware.zip"] {
            let u = directory.appendingPathComponent(n)
            if FileManager.default.fileExists(atPath: u.path) { return u }
        }
        return nil
    }
}

public enum KeysetError: Error, LocalizedError, Equatable {
    case invalidName
    case missingKeys([String])
    case badFirmware
    case exists(String)
    case notFound
    case io(String)

    public var errorDescription: String? {
        switch self {
        case .invalidName: return "Invalid set name. Use letters, digits, spaces, - and _."
        case .missingKeys(let m): return "prod.keys is missing required entries: \(m.joined(separator: ", "))."
        case .badFirmware: return "Firmware not recognised: pick the firmware .zip or a folder containing .nca files."
        case .exists(let n): return "A set named \"\(n)\" already exists."
        case .notFound: return "Set not found."
        case .io(let m): return "File error: \(m)"
        }
    }
}

/// Manages key/firmware sets in app support. Never reads key values out; only key NAMES are parsed.
public final class KeysetManager: @unchecked Sendable {
    public let root: URL
    private var setsDir: URL { root.appendingPathComponent("keysets", isDirectory: true) }
    private let fm = FileManager.default

    public static var defaultRoot: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Napple", isDirectory: true)
    }

    public init(root: URL = KeysetManager.defaultRoot) { self.root = root }

    /// Names that must exist for the core to work (derivable chain or direct header key).
    public static let requiredKeyNames = [
        "aes_kek_generation_source", "aes_key_generation_source",
        "titlekek_source", "key_area_key_application_source", "master_key_00",
    ]

    /// Returns the list of missing required key names (empty means valid). Values are never retained.
    public static func missingKeyNames(in file: URL) throws -> [String] {
        let text: String
        do { text = try String(contentsOf: file, encoding: .utf8) } catch { throw KeysetError.io("cannot read keys file") }
        var present = Set<String>()
        for line in text.split(whereSeparator: \.isNewline) {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("#") { continue }
            guard let eq = t.firstIndex(of: "=") else { continue }
            let name = t[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            let hasValue = !t[t.index(after: eq)...].trimmingCharacters(in: .whitespaces).isEmpty
            if hasValue, !name.isEmpty { present.insert(name) }
        }
        var missing = requiredKeyNames.filter { !present.contains($0) }
        if !present.contains("header_key"), !(present.contains("header_kek_source") && present.contains("header_key_source")) {
            missing.append("header_key")
        }
        return missing
    }

    /// Firmware is a .zip containing .nca entries, or a directory containing .nca files.
    public static func isValidFirmware(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { return false }
        if isDir.boolValue {
            guard let e = FileManager.default.enumerator(at: url, includingPropertiesForKeys: nil) else { return false }
            for case let f as URL in e where f.pathExtension.lowercased() == "nca" { return true }
            return false
        }
        guard url.pathExtension.lowercased() == "zip" else { return false }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        p.arguments = ["-Z1", url.path]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return false }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let s = String(decoding: data, as: UTF8.self)
        return p.terminationStatus == 0 && s.split(separator: "\n").contains { $0.lowercased().hasSuffix(".nca") }
    }

    public static func sanitize(_ name: String) -> String? {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty, n.count <= 48, !n.hasPrefix(".") else { return nil }
        let ok = n.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) || " -_.".unicodeScalars.contains($0) }
        return ok ? n : nil
    }

    public func list() -> [Keyset] {
        let items = (try? fm.contentsOfDirectory(at: setsDir, includingPropertiesForKeys: [.isDirectoryKey])) ?? []
        return items.filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
            .map { Keyset(name: $0.lastPathComponent, directory: $0) }
            .filter { fm.fileExists(atPath: $0.keysURL.path) }
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// Imports (or, with `replace`, updates) a set. Optional parts left nil are kept on replace.
    @discardableResult
    public func save(name: String, keys: URL?, titleKeys: URL?, firmware: URL?, replace: Bool = false) throws -> Keyset {
        guard let clean = Self.sanitize(name) else { throw KeysetError.invalidName }
        let dir = setsDir.appendingPathComponent(clean, isDirectory: true)
        let existed = fm.fileExists(atPath: dir.path)
        if existed && !replace { throw KeysetError.exists(clean) }
        if !existed && keys == nil { throw KeysetError.missingKeys(Self.requiredKeyNames) }
        if let keys {
            let missing = try Self.missingKeyNames(in: keys)
            if !missing.isEmpty { throw KeysetError.missingKeys(missing) }
        }
        if let firmware, !Self.isValidFirmware(firmware) { throw KeysetError.badFirmware }
        do {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            if let keys { try put(keys, to: dir.appendingPathComponent("prod.keys")) }
            if let titleKeys { try put(titleKeys, to: dir.appendingPathComponent("title.keys")) }
            if let firmware {
                for n in ["firmware", "firmware.zip"] { try? fm.removeItem(at: dir.appendingPathComponent(n)) }
                var isDir: ObjCBool = false
                _ = fm.fileExists(atPath: firmware.path, isDirectory: &isDir)
                let dest = dir.appendingPathComponent(isDir.boolValue ? "firmware" : "firmware.zip")
                try put(firmware, to: dest)
            }
        } catch let e as KeysetError { throw e } catch { throw KeysetError.io(error.localizedDescription) }
        return Keyset(name: clean, directory: dir)
    }

    public func delete(_ set: Keyset) throws {
        guard set.directory.deletingLastPathComponent().standardizedFileURL == setsDir.standardizedFileURL else { throw KeysetError.notFound }
        do { try fm.removeItem(at: set.directory) } catch { throw KeysetError.io(error.localizedDescription) }
    }

    // MARK: private
    private func put(_ src: URL, to dest: URL) throws {
        try? fm.removeItem(at: dest)
        try fm.copyItem(at: src, to: dest)
        try lock(dest)
    }

    private func lock(_ url: URL) throws {
        var isDir: ObjCBool = false
        _ = fm.fileExists(atPath: url.path, isDirectory: &isDir)
        try fm.setAttributes([.posixPermissions: isDir.boolValue ? 0o700 : 0o600], ofItemAtPath: url.path)
        if isDir.boolValue, let e = fm.enumerator(at: url, includingPropertiesForKeys: nil) {
            for case let f as URL in e {
                var d: ObjCBool = false
                _ = fm.fileExists(atPath: f.path, isDirectory: &d)
                try fm.setAttributes([.posixPermissions: d.boolValue ? 0o700 : 0o600], ofItemAtPath: f.path)
            }
        }
    }
}
