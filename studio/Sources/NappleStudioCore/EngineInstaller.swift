import Foundation

/// First-launch setup of the Napple "engine" (the `napple` CLI plus the runtime and launcher it packs into games).
///
/// The binaries ship inside NappleStudio.app/Contents/Resources. On first launch (and after every update) they are copied to
/// `<support>/engine/<version>/`, `<support>/engine/current` is pointed at that copy, and the environment is wired up so the
/// engine also works outside Studio:
///   - `napple` is linked into `~/.local/bin`
///   - `<support>/env.sh` exports `NAPPLE_HOME`, `NAPPLE_CLI` and the PATH entry; `~/.zprofile` sources it (one marked block)
///   - `launchctl setenv` makes the same variables visible to GUI apps for the rest of the login session
/// Everything is idempotent and never touches files it did not create.
public struct EngineInstaller: Sendable {
    public static let requiredFiles = ["napple", "napple-runtime", "napple-launcher"]
    public static let optionalFiles = ["napple-runtime-ios", "libnapple-applet.dylib", "matrix.json"]
    public static let profileMarkerBegin = "# >>> napple >>>"
    public static let profileMarkerEnd = "# <<< napple <<<"

    public let resources: URL   // where the bundled binaries live (Contents/Resources)
    public let support: URL     // ~/Library/Application Support/Napple
    public let home: URL        // user home (for ~/.local/bin and the shell profile)
    public var setLaunchctl = true

    public init(resources: URL, support: URL = KeysetManager.defaultRoot,
                home: URL = FileManager.default.homeDirectoryForCurrentUser) {
        self.resources = resources; self.support = support; self.home = home
    }

    public var engineRoot: URL { support.appendingPathComponent("engine", isDirectory: true) }
    public var current: URL { engineRoot.appendingPathComponent("current") }
    public var cli: URL { current.appendingPathComponent("napple") }
    public var envScript: URL { support.appendingPathComponent("env.sh") }
    public var binLink: URL { home.appendingPathComponent(".local/bin/napple") }
    public var zprofile: URL { home.appendingPathComponent(".zprofile") }

    public struct Report: Equatable, Sendable {
        public var version: String
        public var installed: Bool      // false when the right version was already in place
        public var notes: [String]
    }

    /// True when `current` already points at a complete copy of `version`.
    public func isUpToDate(version: String) -> Bool {
        let fm = FileManager.default
        let marker = current.appendingPathComponent(".version")
        guard let v = try? String(contentsOf: marker, encoding: .utf8), v.trimmingCharacters(in: .whitespacesAndNewlines) == version else { return false }
        return Self.requiredFiles.allSatisfy { fm.isExecutableFile(atPath: current.appendingPathComponent($0).path) }
    }

    /// True when the bundle actually carries an engine (false for a bare `swift run` during development).
    public var bundleHasEngine: Bool {
        Self.requiredFiles.allSatisfy { FileManager.default.isExecutableFile(atPath: resources.appendingPathComponent($0).path) }
    }

    @discardableResult
    public func install(version: String) throws -> Report {
        let fm = FileManager.default
        var notes: [String] = []
        guard bundleHasEngine else { throw CLIError.notFound }

        var copied = false
        if !isUpToDate(version: version) {
            try fm.createDirectory(at: engineRoot, withIntermediateDirectories: true)
            let dest = engineRoot.appendingPathComponent(version, isDirectory: true)
            let staging = engineRoot.appendingPathComponent(".staging-\(UUID().uuidString)", isDirectory: true)
            try fm.createDirectory(at: staging, withIntermediateDirectories: true)
            defer { try? fm.removeItem(at: staging) }
            for f in Self.requiredFiles + Self.optionalFiles {
                let src = resources.appendingPathComponent(f)
                guard fm.fileExists(atPath: src.path) else { continue }
                try fm.copyItem(at: src, to: staging.appendingPathComponent(f))
            }
            try version.write(to: staging.appendingPathComponent(".version"), atomically: true, encoding: .utf8)
            if fm.fileExists(atPath: dest.path) { try fm.removeItem(at: dest) }
            try fm.moveItem(at: staging, to: dest)
            try replaceSymlink(at: current, to: version)   // relative target: the engine folder stays relocatable
            copied = true
            pruneOldVersions(keep: version)
        }

        // Command-line access. Never overwrite a real file the user put there.
        do {
            let bin = binLink.deletingLastPathComponent()
            try fm.createDirectory(at: bin, withIntermediateDirectories: true)
            if let attrs = try? fm.attributesOfItem(atPath: binLink.path), (attrs[.type] as? FileAttributeType) != .typeSymbolicLink {
                notes.append("~/.local/bin/napple already exists and was left alone")
            } else {
                try replaceSymlink(at: binLink, to: cli.path)
            }
        } catch { notes.append("could not link napple into ~/.local/bin: \(error.localizedDescription)") }

        // Environment variables for new terminals.
        do {
            try fm.createDirectory(at: support, withIntermediateDirectories: true)
            try envScriptText().write(to: envScript, atomically: true, encoding: .utf8)
            try ensureProfileBlock()
        } catch { notes.append("could not update the shell profile: \(error.localizedDescription)") }

        // ... and for GUI apps in this login session.
        if setLaunchctl { applyLaunchctl() }
        return Report(version: version, installed: copied, notes: notes)
    }

    // MARK: pieces (internal for tests)

    func envScriptText() -> String {
        """
        # Written by Napple Studio. Safe to delete; Studio recreates it.
        export NAPPLE_HOME="\(support.path)"
        export NAPPLE_CLI="\(cli.path)"
        case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) export PATH="$HOME/.local/bin:$PATH" ;; esac

        """
    }

    func profileBlock() -> String {
        "\(Self.profileMarkerBegin)\n[ -f \"\(envScript.path)\" ] && . \"\(envScript.path)\"\n\(Self.profileMarkerEnd)\n"
    }

    /// Appends (or refreshes) the single marked block in ~/.zprofile.
    func ensureProfileBlock() throws {
        let existing = (try? String(contentsOf: zprofile, encoding: .utf8)) ?? ""
        var text = existing
        if let b = existing.range(of: Self.profileMarkerBegin), let e = existing.range(of: Self.profileMarkerEnd, range: b.upperBound..<existing.endIndex) {
            var end = e.upperBound
            if end < existing.endIndex, existing[end] == "\n" { end = existing.index(after: end) }
            text.replaceSubrange(b.lowerBound..<end, with: profileBlock())
        } else {
            if !text.isEmpty && !text.hasSuffix("\n") { text += "\n" }
            text += (text.isEmpty ? "" : "\n") + profileBlock()
        }
        if text != existing { try text.write(to: zprofile, atomically: true, encoding: .utf8) }
    }

    private func replaceSymlink(at link: URL, to target: String) throws {
        let fm = FileManager.default
        if (try? fm.destinationOfSymbolicLink(atPath: link.path)) != nil || fm.fileExists(atPath: link.path) { try fm.removeItem(at: link) }
        try fm.createSymbolicLink(atPath: link.path, withDestinationPath: target)
    }

    private func pruneOldVersions(keep: String) {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: engineRoot, includingPropertiesForKeys: [.contentModificationDateKey]) else { return }
        let old = items.filter { $0.lastPathComponent != keep && $0.lastPathComponent != "current" && !$0.lastPathComponent.hasPrefix(".") }
            .sorted { ((try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) >
                      ((try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) }
        for u in old.dropFirst(1) { try? fm.removeItem(at: u) }   // keep the previous version for rollback
    }

    private func applyLaunchctl() {
        for (k, v) in [("NAPPLE_HOME", support.path), ("NAPPLE_CLI", cli.path)] {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
            p.arguments = ["setenv", k, v]
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            try? p.run(); p.waitUntilExit()
        }
    }
}
