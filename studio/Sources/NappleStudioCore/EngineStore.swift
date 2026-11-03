import CryptoKit
import Foundation

/// One release of the Napple repo on GitHub, reduced to what Studio needs to offer it as an engine.
public struct EngineRelease: Identifiable, Equatable, Sendable {
    public struct Asset: Equatable, Sendable {
        public var name: String
        public var size: Int64
        public var url: URL
        public init(name: String, size: Int64, url: URL) { self.name = name; self.size = size; self.url = url }
    }
    public var tag: String
    /// The tag without a leading `v`: also the folder name under `<support>/engine/`.
    public var version: String
    public var title: String
    public var publishedAt: Date?
    public var prerelease: Bool
    /// `Napple-macOS-arm64.zip`; nil when the release has no macOS build.
    public var zip: Asset?
    /// `SHA256SUMS.txt`, when published.
    public var sums: Asset?
    public var id: String { tag }
    public var installable: Bool { zip != nil && EngineStore.folderName(for: version) != nil }

    public init(tag: String, version: String, title: String = "", publishedAt: Date? = nil, prerelease: Bool = false, zip: Asset? = nil, sums: Asset? = nil) {
        self.tag = tag; self.version = version; self.title = title; self.publishedAt = publishedAt; self.prerelease = prerelease; self.zip = zip; self.sums = sums
    }
}

public enum EngineStoreError: Error, LocalizedError, Equatable {
    case offline(String)
    case http(Int)
    case rateLimited
    case badResponse
    case noAsset(String)
    case invalidVersion(String)
    case alreadyInstalled(String)
    case checksumMismatch
    case checksumUnlisted(String)
    case checksumUnreadable
    case extractFailed(String)
    case missingFiles([String])
    case notRemovable(String)
    case io(String)

    public var errorDescription: String? {
        switch self {
        case .offline(let m): return "Could not reach GitHub: \(m)"
        case .http(let c): return "GitHub answered with HTTP \(c)."
        case .rateLimited: return "GitHub is limiting requests from this network. Try again in a few minutes."
        case .badResponse: return "GitHub sent an answer Studio could not read."
        case .noAsset(let n): return "This release has no \(n) to install."
        case .invalidVersion(let v): return "\"\(v)\" is not a usable version name."
        case .alreadyInstalled(let v): return "Version \(v) is already installed."
        case .checksumMismatch: return "The download does not match its published checksum, so it was discarded."
        case .checksumUnlisted(let n): return "The release's SHA256SUMS.txt does not list \(n), so the download was discarded."
        case .checksumUnreadable: return "The release's SHA256SUMS.txt could not be read, so the download was discarded."
        case .extractFailed(let m): return "The download could not be unpacked: \(m)"
        case .missingFiles(let f): return "The download is missing \(f.joined(separator: ", "))."
        case .notRemovable(let why): return why
        case .io(let m): return "File error: \(m)"
        }
    }
}

public enum EngineInstallStage: Equatable, Sendable {
    case downloading(Double)   // 0...1, or -1 when the size is unknown
    case verifying
    case extracting
    case installing
}

/// The network, injectable so tests never touch it.
public struct EngineNetwork: Sendable {
    /// GET a small document: body and HTTP status.
    public var get: @Sendable (URL) async throws -> (Data, Int)
    /// Download `url` to the file `destination` (replaced when present); `progress(written, expectedTotal)`; throws on HTTP errors and when cancelled.
    public var download: @Sendable (URL, URL, @escaping @Sendable (Int64, Int64) -> Void) async throws -> Void
    public init(get: @escaping @Sendable (URL) async throws -> (Data, Int),
                download: @escaping @Sendable (URL, URL, @escaping @Sendable (Int64, Int64) -> Void) async throws -> Void) {
        self.get = get; self.download = download
    }

    /// URLSession, ephemeral (no cookies, no cache on disk), with timeouts. Unauthenticated.
    public static let live = EngineNetwork(
        get: { url in
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 15; cfg.timeoutIntervalForResource = 30; cfg.waitsForConnectivity = false
            let session = URLSession(configuration: cfg)
            defer { session.finishTasksAndInvalidate() }
            var req = URLRequest(url: url)
            req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            req.setValue("NappleStudio", forHTTPHeaderField: "User-Agent")
            let (data, resp) = try await session.data(for: req)
            return (data, (resp as? HTTPURLResponse)?.statusCode ?? 0)
        },
        download: { url, dest, progress in
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 30; cfg.timeoutIntervalForResource = 3600
            let delegate = DownloadDelegate(destination: dest, progress: progress)
            let session = URLSession(configuration: cfg, delegate: delegate, delegateQueue: nil)
            defer { session.finishTasksAndInvalidate() }
            var req = URLRequest(url: url)
            req.setValue("NappleStudio", forHTTPHeaderField: "User-Agent")
            let task = session.downloadTask(with: req)
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                    delegate.start(c)
                    task.resume()
                }
            } onCancel: { task.cancel() }
        }
    )
}

private final class DownloadDelegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    let destination: URL
    let progress: @Sendable (Int64, Int64) -> Void
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Void, Error>?
    private var failure: Error?

    init(destination: URL, progress: @escaping @Sendable (Int64, Int64) -> Void) { self.destination = destination; self.progress = progress }

    func start(_ c: CheckedContinuation<Void, Error>) { lock.lock(); continuation = c; lock.unlock() }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        progress(totalBytesWritten, totalBytesExpectedToWrite)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            lock.lock(); failure = EngineStoreError.http(http.statusCode); lock.unlock()
            return
        }
        do {
            try? FileManager.default.removeItem(at: destination)
            try FileManager.default.moveItem(at: location, to: destination)   // must happen here: the system deletes `location` right after this call
        } catch {
            lock.lock(); failure = EngineStoreError.io(error.localizedDescription); lock.unlock()
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        lock.lock()
        let c = continuation; continuation = nil
        let f = failure
        lock.unlock()
        if let f { c?.resume(throwing: f) }
        else if let e = error as? URLError, e.code == .cancelled { c?.resume(throwing: CancellationError()) }
        else if let error { c?.resume(throwing: EngineStoreError.offline(error.localizedDescription)) }
        else { c?.resume() }
    }
}

/// Engines published on GitHub: list the releases, download one, check it, unpack it, install it next to the others, remove installed ones.
/// It never touches `engine/current`, `~/.zprofile` or the engine bundled in Studio.
public struct EngineStore: Sendable {
    public static let repo = "erostalevi/napple"
    public static let assetName = "Napple-macOS-arm64.zip"
    public static let sumsName = "SHA256SUMS.txt"
    public static var releasesURL: URL { URL(string: "https://api.github.com/repos/\(repo)/releases?per_page=15")! }

    public let engineRoot: URL
    public var network: EngineNetwork
    public var tools: ToolRunner
    public var keep: Int

    public init(engineRoot: URL, network: EngineNetwork = .live, tools: ToolRunner = .system, keep: Int = EngineInstaller.keptVersions) {
        self.engineRoot = engineRoot; self.network = network; self.tools = tools; self.keep = keep
    }

    // MARK: releases

    public func fetchReleases() async throws -> [EngineRelease] {
        let data: Data, status: Int
        do { (data, status) = try await network.get(Self.releasesURL) }
        catch is CancellationError { throw CancellationError() }
        catch let e as EngineStoreError { throw e }
        catch { throw EngineStoreError.offline(error.localizedDescription) }
        switch status {
        case 200..<300: break
        case 403, 429: throw EngineStoreError.rateLimited
        default: throw EngineStoreError.http(status)
        }
        return try Self.parseReleases(data)
    }

    private struct RawAsset: Decodable { var name: String; var size: Int64?; var browser_download_url: String }
    private struct RawRelease: Decodable {
        var tag_name: String?; var name: String?; var draft: Bool?; var prerelease: Bool?; var published_at: String?; var assets: [RawAsset]?
    }

    /// GitHub's `releases` array. Drafts and entries without a tag are dropped; newest first (as GitHub sends them, re-sorted by date when dates exist).
    public static func parseReleases(_ data: Data) throws -> [EngineRelease] {
        guard let raw = try? JSONDecoder().decode([RawRelease].self, from: data) else { throw EngineStoreError.badResponse }
        let iso = ISO8601DateFormatter()
        let out: [EngineRelease] = raw.compactMap { r in
            guard let tag = r.tag_name, !tag.isEmpty, r.draft != true else { return nil }
            func asset(_ n: String) -> EngineRelease.Asset? {
                r.assets?.first { $0.name == n }.flatMap { a in URL(string: a.browser_download_url).map { EngineRelease.Asset(name: a.name, size: a.size ?? 0, url: $0) } }
            }
            let version = tag.hasPrefix("v") || tag.hasPrefix("V") ? String(tag.dropFirst()) : tag
            return EngineRelease(tag: tag, version: version, title: r.name ?? tag, publishedAt: r.published_at.flatMap { iso.date(from: $0) },
                                 prerelease: r.prerelease ?? false, zip: asset(assetName), sums: asset(sumsName))
        }
        return out.enumerated().sorted { a, b in
            switch (a.element.publishedAt, b.element.publishedAt) {
            case let (x?, y?) where x != y: return x > y
            default: return a.offset < b.offset
            }
        }.map(\.element)
    }

    /// The newest release that is not a pre-release (the one marked "Latest").
    public static func latest(in releases: [EngineRelease]) -> EngineRelease? { releases.first { !$0.prerelease && $0.installable } ?? releases.first { $0.installable } }

    /// A version as a folder name, or nil when it could be abused as a path or collides with Studio's own names.
    public static func folderName(for version: String) -> String? {
        guard version.range(of: #"^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$"#, options: .regularExpression) != nil,
              !version.contains(".."), version != "current", version != EngineBuild.bundledID else { return nil }
        return version
    }

    // MARK: checksums

    /// `shasum -a 256` output: `<64 hex>  <name>` (a `*` before the name marks binary mode).
    public static func parseSums(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            let parts = line.split(maxSplits: 1, whereSeparator: { $0 == " " || $0 == "\t" })
            guard parts.count == 2, parts[0].count == 64, parts[0].allSatisfy(\.isHexDigit) else { continue }
            var name = parts[1].trimmingCharacters(in: .whitespaces)
            if name.hasPrefix("*") { name.removeFirst() }
            out[name] = parts[0].lowercased()
        }
        return out
    }

    public static func sha256(of file: URL) throws -> String {
        guard let h = try? FileHandle(forReadingFrom: file) else { throw EngineStoreError.io("cannot read \(file.lastPathComponent)") }
        defer { try? h.close() }
        var hasher = SHA256()
        while let chunk = try? h.read(upToCount: 1 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // MARK: install

    public struct InstallResult: Equatable, Sendable {
        public var version: String
        public var folder: URL
        public var pruned: [String]
    }

    /// Files an engine folder needs next to `napple`: required ones, then every optional one the download carries.
    static var engineFiles: [String] { EngineInstaller.requiredFiles + EngineInstaller.optionalFiles }

    /// `protecting`: folder names that must survive pruning (the engine the user picked).
    public func install(_ release: EngineRelease, protecting: Set<String> = [], progress: @escaping @Sendable (EngineInstallStage) -> Void = { _ in }) async throws -> InstallResult {
        guard let zip = release.zip else { throw EngineStoreError.noAsset(Self.assetName) }
        guard let name = Self.folderName(for: release.version) else { throw EngineStoreError.invalidVersion(release.version) }
        let fm = FileManager.default
        let dest = engineRoot.appendingPathComponent(name, isDirectory: true)
        if fm.fileExists(atPath: dest.path) { throw EngineStoreError.alreadyInstalled(name) }
        do { try fm.createDirectory(at: engineRoot, withIntermediateDirectories: true) } catch { throw EngineStoreError.io(error.localizedDescription) }
        let staging = engineRoot.appendingPathComponent(".staging-\(UUID().uuidString)", isDirectory: true)
        do { try fm.createDirectory(at: staging, withIntermediateDirectories: true) } catch { throw EngineStoreError.io(error.localizedDescription) }
        defer { try? fm.removeItem(at: staging) }

        // download
        let file = staging.appendingPathComponent(Self.assetName)
        progress(.downloading(-1))
        do {
            try await network.download(zip.url, file) { written, total in
                progress(.downloading(total > 0 ? min(1, Double(written) / Double(total)) : -1))
            }
        } catch is CancellationError { throw CancellationError() }
        catch let e as EngineStoreError { throw e }
        catch { throw EngineStoreError.offline(error.localizedDescription) }
        try Task.checkCancellation()

        // verify
        progress(.verifying)
        if let sums = release.sums {
            let body: Data, status: Int
            do { (body, status) = try await network.get(sums.url) } catch is CancellationError { throw CancellationError() } catch { throw EngineStoreError.checksumUnreadable }
            guard (200..<300).contains(status), let text = String(data: body, encoding: .utf8) else { throw EngineStoreError.checksumUnreadable }
            guard let expected = Self.parseSums(text)[Self.assetName] else { throw EngineStoreError.checksumUnlisted(Self.assetName) }
            guard try Self.sha256(of: file) == expected else { throw EngineStoreError.checksumMismatch }
        }
        try Task.checkCancellation()

        // unpack
        progress(.extracting)
        let unpacked = staging.appendingPathComponent("unpacked", isDirectory: true)
        do {
            try fm.createDirectory(at: unpacked, withIntermediateDirectories: true)
            _ = try await tools.run(URL(fileURLWithPath: "/usr/bin/ditto"), ["-x", "-k", file.path, unpacked.path])
        } catch is CancellationError { throw CancellationError() } catch { throw EngineStoreError.extractFailed(error.localizedDescription) }
        try Task.checkCancellation()

        // stage the engine folder exactly like EngineInstaller.install, then move it into place
        progress(.installing)
        let bin = try Self.locateBin(in: unpacked)
        let missing = EngineInstaller.requiredFiles.filter { !Self.isRegularFile(bin.appendingPathComponent($0)) }
        if !missing.isEmpty { throw EngineStoreError.missingFiles(missing) }
        let pkg = staging.appendingPathComponent("engine", isDirectory: true)
        do {
            try fm.createDirectory(at: pkg, withIntermediateDirectories: true)
            for f in Self.engineFiles where Self.isRegularFile(bin.appendingPathComponent(f)) {
                let to = pkg.appendingPathComponent(f)
                try fm.copyItem(at: bin.appendingPathComponent(f), to: to)
                let executable = !(f.hasSuffix(".dylib") || f.hasSuffix(".json"))
                try fm.setAttributes([.posixPermissions: executable ? 0o755 : 0o644], ofItemAtPath: to.path)
            }
            try name.write(to: pkg.appendingPathComponent(".version"), atomically: true, encoding: .utf8)
            if fm.fileExists(atPath: dest.path) { throw EngineStoreError.alreadyInstalled(name) }
            try fm.moveItem(at: pkg, to: dest)
        } catch let e as EngineStoreError { throw e } catch { throw EngineStoreError.io(error.localizedDescription) }
        let pruned = prune(keeping: name, protecting: protecting)
        return InstallResult(version: name, folder: dest, pruned: pruned)
    }

    static func isRegularFile(_ u: URL) -> Bool {
        (try? FileManager.default.attributesOfItem(atPath: u.path))?[.type] as? FileAttributeType == .typeRegular
    }

    /// `Napple-<ver>/bin` inside the unpacked archive (also accepts `bin` at the top, or one more level down).
    static func locateBin(in root: URL) throws -> URL {
        let fm = FileManager.default
        func dirs(_ u: URL) -> [URL] {
            ((try? fm.contentsOfDirectory(at: u, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles])) ?? [])
                .filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true && $0.lastPathComponent != "__MACOSX" }
                .sorted { $0.lastPathComponent.hasPrefix("Napple-") && !$1.lastPathComponent.hasPrefix("Napple-") }
        }
        let level1 = dirs(root)
        let candidates = [root] + level1 + level1.flatMap { dirs($0) }
        for d in candidates {
            let bin = d.appendingPathComponent("bin", isDirectory: true)
            if isRegularFile(bin.appendingPathComponent("napple")) { return bin }
        }
        throw EngineStoreError.missingFiles(["bin/napple"])
    }

    /// Keeps the newest `keep` installed versions (never `current`'s target, the new one or a protected one); returns what it removed.
    @discardableResult
    func prune(keeping new: String, protecting: Set<String>) -> [String] {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: engineRoot, includingPropertiesForKeys: [.contentModificationDateKey]) else { return [] }
        let active = engineRoot.appendingPathComponent("current").resolvingSymlinksInPath().standardizedFileURL.lastPathComponent
        let versions = items.filter { $0.lastPathComponent != "current" && !$0.lastPathComponent.hasPrefix(".") && Self.isDirectory($0) }
            .sorted { Self.modified($0) > Self.modified($1) }
        var removed: [String] = []
        for u in versions.dropFirst(max(keep, 1)) {
            let n = u.lastPathComponent
            if n == new || n == active || protecting.contains(n) { continue }
            if (try? fm.removeItem(at: u)) != nil { removed.append(n) }
        }
        return removed
    }

    private static func isDirectory(_ u: URL) -> Bool { (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
    private static func modified(_ u: URL) -> Date { (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }

    // MARK: remove

    /// Deletes one installed version folder. Refuses the bundled engine, `current` (the link and what it points at) and any folder that is not a direct child of the engine root.
    public func remove(_ build: EngineBuild) throws {
        if build.isBundled || build.id == EngineBuild.bundledID { throw EngineStoreError.notRemovable("Studio's own engine is part of the app and cannot be removed.") }
        try remove(folder: build.folder)
    }

    public func remove(folder: URL) throws {
        let fm = FileManager.default
        let root = engineRoot.resolvingSymlinksInPath().standardizedFileURL
        let name = folder.lastPathComponent
        guard name != "current", name != EngineBuild.bundledID, !name.hasPrefix("."), !name.isEmpty else { throw EngineStoreError.notRemovable("That engine cannot be removed.") }
        // the folder must be a real directory (not a link) sitting directly in the engine root
        let direct = engineRoot.appendingPathComponent(name, isDirectory: true)
        guard folder.deletingLastPathComponent().resolvingSymlinksInPath().standardizedFileURL == root,
              (try? fm.destinationOfSymbolicLink(atPath: direct.path)) == nil,
              Self.isDirectory(direct) else { throw EngineStoreError.notRemovable("That folder is not an installed engine.") }
        let active = engineRoot.appendingPathComponent("current").resolvingSymlinksInPath().standardizedFileURL
        if direct.resolvingSymlinksInPath().standardizedFileURL == active {
            throw EngineStoreError.notRemovable("This version is the one the Terminal's napple uses. Install or activate another version first.")
        }
        do { try fm.removeItem(at: direct) } catch { throw EngineStoreError.io(error.localizedDescription) }
    }
}
