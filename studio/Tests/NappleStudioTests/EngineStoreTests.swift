import Foundation
import Testing
@testable import NappleStudioCore

private let sampleReleases = """
[
  {"url":"https://api.github.com/repos/erostalevi/Napple/releases/2","tag_name":"v0.1.39","name":"Napple 0.1.39","draft":false,"prerelease":false,
   "published_at":"2026-11-03T01:57:07Z","author":{"login":"github-actions[bot]"},
   "assets":[
     {"name":"Napple-macOS-arm64.zip","size":20094057,"browser_download_url":"https://github.com/erostalevi/Napple/releases/download/v0.1.39/Napple-macOS-arm64.zip"},
     {"name":"Napple-Studio.dmg","size":11703894,"browser_download_url":"https://github.com/erostalevi/Napple/releases/download/v0.1.39/Napple-Studio.dmg"},
     {"name":"SHA256SUMS.txt","size":173,"browser_download_url":"https://github.com/erostalevi/Napple/releases/download/v0.1.39/SHA256SUMS.txt"}]},
  {"tag_name":"v0.1.40-rc1","name":"Napple 0.1.40 rc1","draft":false,"prerelease":true,"published_at":"2026-11-03T03:00:00Z",
   "assets":[{"name":"Napple-macOS-arm64.zip","size":5,"browser_download_url":"https://github.com/erostalevi/Napple/releases/download/v0.1.40-rc1/Napple-macOS-arm64.zip"}]},
  {"tag_name":"v0.1.38","name":"Napple 0.1.38","draft":false,"prerelease":false,"published_at":"2026-11-03T01:25:53Z",
   "assets":[{"name":"Napple-Studio.dmg","size":11696274,"browser_download_url":"https://github.com/erostalevi/Napple/releases/download/v0.1.38/Napple-Studio.dmg"}]},
  {"tag_name":"v0.1.37","name":"draft","draft":true,"prerelease":false,"published_at":"2026-11-02T00:00:00Z","assets":[]},
  {"name":"no tag","draft":false}
]
"""

/// A fake release archive: Napple-<ver>/bin/<files> zipped with ditto, like the real Napple-macOS-arm64.zip.
private func makeZip(version: String, in dir: URL, omit: String? = nil, extraTopLevel: Bool = true) throws -> URL {
    let fm = FileManager.default
    let top = dir.appendingPathComponent("build-\(UUID().uuidString)/Napple-\(version)", isDirectory: true)
    let bin = top.appendingPathComponent("bin", isDirectory: true)
    try fm.createDirectory(at: bin, withIntermediateDirectories: true)
    for f in ["napple", "napple-runtime", "napple-launcher", "libnapple-applet.dylib", "matrix.json"] where f != omit {
        try Data("\(f) \(version)".utf8).write(to: bin.appendingPathComponent(f))
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: bin.appendingPathComponent(f).path)   // the install must chmod +x
    }
    if extraTopLevel { try Data("readme".utf8).write(to: top.appendingPathComponent("README.txt")) }
    let zip = dir.appendingPathComponent("zip-\(UUID().uuidString).zip")
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
    p.arguments = ["-c", "-k", "--keepParent", top.path, zip.path]
    try p.run(); p.waitUntilExit()
    #expect(p.terminationStatus == 0)
    return zip
}

private func release(_ version: String, sums: Bool, zip: Bool = true) -> EngineRelease {
    let u = { (n: String) in URL(string: "https://example.invalid/\(version)/\(n)")! }
    return EngineRelease(tag: "v\(version)", version: version, zip: zip ? .init(name: EngineStore.assetName, size: 100, url: u(EngineStore.assetName)) : nil,
                         sums: sums ? .init(name: EngineStore.sumsName, size: 10, url: u(EngineStore.sumsName)) : nil)
}

/// Serves `zip` for every download and `sums` for every get; records what was asked.
private final class Fake: @unchecked Sendable {
    var zip: URL
    var sumsBody: String?
    var sumsStatus = 200
    var downloads: [URL] = []
    init(zip: URL, sums: String? = nil) { self.zip = zip; self.sumsBody = sums }
    var network: EngineNetwork {
        EngineNetwork(
            get: { [self] _ in (Data((sumsBody ?? "").utf8), sumsStatus) },
            download: { [self] url, dest, progress in
                downloads.append(url)
                try FileManager.default.copyItem(at: zip, to: dest)
                let size = (try? FileManager.default.attributesOfItem(atPath: dest.path)[.size] as? Int64) ?? 0
                progress(size / 2, size); progress(size, size)
            })
    }
}

private func sums(for zip: URL, name: String = EngineStore.assetName) throws -> String {
    "\(try EngineStore.sha256(of: zip))  \(name)\n0000000000000000000000000000000000000000000000000000000000000000  Napple-Studio.dmg\n"
}

private final class StageLog: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [EngineInstallStage] = []
    func add(_ s: EngineInstallStage) { lock.lock(); items.append(s); lock.unlock() }
    var all: [EngineInstallStage] { lock.lock(); defer { lock.unlock() }; return items }
}

@Suite struct EngineStoreTests {
    // MARK: parsing

    @Test func parsesARecordedReleaseList() throws {
        let rs = try EngineStore.parseReleases(Data(sampleReleases.utf8))
        #expect(rs.map(\.tag) == ["v0.1.40-rc1", "v0.1.39", "v0.1.38"])   // draft and tag-less entries dropped, newest first
        let r = try #require(rs.first { $0.tag == "v0.1.39" })
        #expect(r.version == "0.1.39" && r.title == "Napple 0.1.39" && !r.prerelease)
        #expect(r.zip?.size == 20_094_057 && r.zip?.name == "Napple-macOS-arm64.zip")
        #expect(r.zip?.url.absoluteString == "https://github.com/erostalevi/Napple/releases/download/v0.1.39/Napple-macOS-arm64.zip")
        #expect(r.sums?.name == "SHA256SUMS.txt")
        #expect(r.publishedAt == ISO8601DateFormatter().date(from: "2026-11-03T01:57:07Z"))
        #expect(r.installable)
        let old = try #require(rs.first { $0.tag == "v0.1.38" })
        #expect(old.zip == nil && !old.installable)   // only the dmg: nothing to install
        #expect(rs.first?.prerelease == true)
    }

    @Test func latestSkipsPrereleasesAndReleasesWithoutBuild() throws {
        let rs = try EngineStore.parseReleases(Data(sampleReleases.utf8))
        #expect(EngineStore.latest(in: rs)?.tag == "v0.1.39")
        #expect(EngineStore.latest(in: []) == nil)
    }

    @Test func garbageIsABadResponse() {
        #expect(throws: EngineStoreError.badResponse) { try EngineStore.parseReleases(Data("{\"message\":\"Not Found\"}".utf8)) }
        #expect(throws: EngineStoreError.badResponse) { try EngineStore.parseReleases(Data("<html>".utf8)) }
        #expect((try? EngineStore.parseReleases(Data("[]".utf8)))?.isEmpty == true)
    }

    @Test func parsesChecksumFiles() {
        let h1 = String(repeating: "ab", count: 32), h2 = String(repeating: "CD", count: 32)
        let m = EngineStore.parseSums("\(h1)  Napple-macOS-arm64.zip\n\(h2) *Napple-Studio.dmg\nnot a checksum line\n12345  short.zip\n")
        #expect(m["Napple-macOS-arm64.zip"] == h1)
        #expect(m["Napple-Studio.dmg"] == h2.lowercased())
        #expect(m.count == 2)
    }

    @Test func versionsBecomeSafeFolderNames() {
        #expect(EngineStore.folderName(for: "0.1.39") == "0.1.39")
        #expect(EngineStore.folderName(for: "0.2.0-rc1+5") == "0.2.0-rc1+5")
        for bad in ["", "..", "../x", "a/b", "current", "bundled", ".hidden", "a b", "0.1..2", String(repeating: "1", count: 65)] {
            #expect(EngineStore.folderName(for: bad) == nil, "\(bad)")
        }
    }

    // MARK: fetching

    @Test func fetchMapsHttpAndNetworkFailures() async {
        func store(_ g: @escaping @Sendable (URL) async throws -> (Data, Int)) -> EngineStore {
            EngineStore(engineRoot: tmp(), network: EngineNetwork(get: g, download: { _, _, _ in }))
        }
        let ok = store { _ in (Data(sampleReleases.utf8), 200) }
        #expect((try? await ok.fetchReleases())?.count == 3)
        await #expect(throws: EngineStoreError.rateLimited) { try await store { _ in (Data(), 403) }.fetchReleases() }
        await #expect(throws: EngineStoreError.http(500)) { try await store { _ in (Data(), 500) }.fetchReleases() }
        await #expect(throws: EngineStoreError.badResponse) { try await store { _ in (Data("nope".utf8), 200) }.fetchReleases() }
        do { _ = try await store { _ in throw URLError(.notConnectedToInternet) }.fetchReleases(); Issue.record("should fail") }
        catch let e as EngineStoreError { if case .offline = e {} else { Issue.record("wrong error \(e)") } } catch { Issue.record("wrong error \(error)") }
    }

    @Test func theReleasesURLIsTheRepoWithFifteenPerPage() {
        #expect(EngineStore.releasesURL.absoluteString == "https://api.github.com/repos/erostalevi/napple/releases?per_page=15")
    }

    // MARK: install

    @Test func installsAVerifiedDownloadWithoutTouchingCurrent() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let fm = FileManager.default
        try fm.createDirectory(at: root.appendingPathComponent("0.0.1"), withIntermediateDirectories: true)
        try fm.createSymbolicLink(atPath: root.appendingPathComponent("current").path, withDestinationPath: "0.0.1")
        let zip = try makeZip(version: "0.1.39", in: work)
        let fake = Fake(zip: zip, sums: try sums(for: zip))
        let store = EngineStore(engineRoot: root, network: fake.network)
        let stages = StageLog()
        let res = try await store.install(release("0.1.39", sums: true)) { s in stages.add(s) }

        #expect(res.version == "0.1.39" && res.folder.lastPathComponent == "0.1.39")
        for f in ["napple", "napple-runtime", "napple-launcher"] { #expect(fm.isExecutableFile(atPath: res.folder.appendingPathComponent(f).path), "\(f)") }
        #expect(fm.fileExists(atPath: res.folder.appendingPathComponent("libnapple-applet.dylib").path))
        #expect(fm.fileExists(atPath: res.folder.appendingPathComponent("matrix.json").path))
        #expect(!fm.fileExists(atPath: res.folder.appendingPathComponent("README.txt").path))
        #expect(try String(contentsOf: res.folder.appendingPathComponent(".version"), encoding: .utf8) == "0.1.39")
        #expect(try String(contentsOf: res.folder.appendingPathComponent("napple"), encoding: .utf8) == "napple 0.1.39")
        // `current` still points where it did; no staging left over
        #expect(try fm.destinationOfSymbolicLink(atPath: root.appendingPathComponent("current").path) == "0.0.1")
        #expect(try fm.contentsOfDirectory(atPath: root.path).filter { $0.hasPrefix(".") }.isEmpty)
        // it shows up as an installed engine
        let listed = EngineInstaller(resources: work.appendingPathComponent("none"), support: root.deletingLastPathComponent()).listEngines()
        #expect(listed.contains { $0.id == "0.1.39" && !$0.isBundled && !$0.isActive })
        #expect(fake.downloads.count == 1)
        let seen = stages.all
        #expect(seen.contains(.verifying) && seen.contains(.extracting) && seen.contains(.installing))
        #expect(seen.contains { if case .downloading(let f) = $0 { return f == 1 } else { return false } })
    }

    @Test func aChecksumMismatchIsRejectedAndNothingIsInstalled() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let zip = try makeZip(version: "1.0.0", in: work)
        let wrong = String(repeating: "0", count: 64) + "  \(EngineStore.assetName)\n"
        let store = EngineStore(engineRoot: root, network: Fake(zip: zip, sums: wrong).network)
        await #expect(throws: EngineStoreError.checksumMismatch) { try await store.install(release("1.0.0", sums: true)) }
        let left = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        #expect(left.isEmpty, "\(left)")
    }

    @Test func aReleaseWhoseSumsDoNotListTheZipIsRejected() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let zip = try makeZip(version: "1.0.0", in: work)
        let store = EngineStore(engineRoot: root, network: Fake(zip: zip, sums: try sums(for: zip, name: "Other.zip")).network)
        await #expect(throws: EngineStoreError.checksumUnlisted(EngineStore.assetName)) { try await store.install(release("1.0.0", sums: true)) }
        #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("1.0.0").path))
    }

    @Test func unreadableSumsAreRejected() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let fake = Fake(zip: try makeZip(version: "1.0.0", in: work), sums: "x")
        fake.sumsStatus = 404
        await #expect(throws: EngineStoreError.checksumUnreadable) { try await EngineStore(engineRoot: root, network: fake.network).install(release("1.0.0", sums: true)) }
    }

    @Test func withoutPublishedSumsTheInstallStillWorks() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let store = EngineStore(engineRoot: root, network: Fake(zip: try makeZip(version: "2.0.0", in: work)).network)
        let res = try await store.install(release("2.0.0", sums: false))
        #expect(FileManager.default.isExecutableFile(atPath: res.folder.appendingPathComponent("napple").path))
    }

    @Test func aReleaseWithoutTheMacZipCannotBeInstalled() async {
        let root = tmp().appendingPathComponent("engine")
        let store = EngineStore(engineRoot: root, network: Fake(zip: URL(fileURLWithPath: "/nonexistent")).network)
        await #expect(throws: EngineStoreError.noAsset(EngineStore.assetName)) { try await store.install(release("3.0.0", sums: false, zip: false)) }
        #expect(!FileManager.default.fileExists(atPath: root.path))
    }

    @Test func anArchiveMissingTheLauncherIsRejected() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let store = EngineStore(engineRoot: root, network: Fake(zip: try makeZip(version: "1.0.0", in: work, omit: "napple-launcher")).network)
        await #expect(throws: EngineStoreError.missingFiles(["napple-launcher"])) { try await store.install(release("1.0.0", sums: false)) }
        #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("1.0.0").path))
    }

    @Test func aNonArchiveFailsToExtract() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let bogus = work.appendingPathComponent("bogus.zip")
        try Data("this is not a zip".utf8).write(to: bogus)
        let store = EngineStore(engineRoot: root, network: Fake(zip: bogus).network)
        do { _ = try await store.install(release("1.0.0", sums: false)); Issue.record("should have failed") }
        catch let e as EngineStoreError { if case .extractFailed = e {} else { Issue.record("wrong error \(e)") } }
    }

    @Test func anInstalledVersionIsNeverReplaced() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let installed = root.appendingPathComponent("1.0.0")
        try FileManager.default.createDirectory(at: installed, withIntermediateDirectories: true)
        try Data("mine".utf8).write(to: installed.appendingPathComponent("marker"))
        let store = EngineStore(engineRoot: root, network: Fake(zip: try makeZip(version: "1.0.0", in: work)).network)
        await #expect(throws: EngineStoreError.alreadyInstalled("1.0.0")) { try await store.install(release("1.0.0", sums: false)) }
        #expect(FileManager.default.fileExists(atPath: installed.appendingPathComponent("marker").path))
    }

    @Test func unsafeVersionNamesAreRefused() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let store = EngineStore(engineRoot: root, network: Fake(zip: try makeZip(version: "1.0.0", in: work)).network)
        for bad in ["../evil", "current", "bundled"] {
            await #expect(throws: EngineStoreError.invalidVersion(bad)) { try await store.install(release(bad, sums: false)) }
        }
    }

    @Test func downloadFailureSurfacesAndLeavesNothing() async {
        let root = tmp().appendingPathComponent("engine")
        let net = EngineNetwork(get: { _ in (Data(), 200) }, download: { _, _, _ in throw URLError(.timedOut) })
        do { _ = try await EngineStore(engineRoot: root, network: net).install(release("1.0.0", sums: false)); Issue.record("should fail") }
        catch let e as EngineStoreError { if case .offline = e {} else { Issue.record("wrong error \(e)") } } catch { Issue.record("wrong error \(error)") }
        #expect(((try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []).isEmpty)
    }

    @Test func cancellingStopsTheInstall() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let net = EngineNetwork(get: { _ in (Data(), 200) }, download: { _, _, _ in try await Task.sleep(nanoseconds: 30_000_000_000) })
        let store = EngineStore(engineRoot: root, network: net)
        _ = work
        let t = Task { try await store.install(release("1.0.0", sums: false)) }
        try await Task.sleep(nanoseconds: 200_000_000)
        t.cancel()
        do { _ = try await t.value; Issue.record("should be cancelled") } catch is CancellationError {} catch { Issue.record("wrong error \(error)") }
        #expect(((try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []).isEmpty)
    }

    @Test func installingPrunesToTheNewestFiveAndSparesCurrentAndProtected() async throws {
        let work = tmp(), root = tmp().appendingPathComponent("engine")
        let fm = FileManager.default
        for (i, v) in ["a0", "a1", "a2", "a3", "a4", "a5", "a6"].enumerated() {
            let d = root.appendingPathComponent(v)
            try fm.createDirectory(at: d, withIntermediateDirectories: true)
            try fm.setAttributes([.modificationDate: Date(timeIntervalSinceNow: Double(-1000 + i * 10))], ofItemAtPath: d.path)
        }
        try fm.createSymbolicLink(atPath: root.appendingPathComponent("current").path, withDestinationPath: "a1")   // the oldest is in use
        let store = EngineStore(engineRoot: root, network: Fake(zip: try makeZip(version: "9.9.9", in: work)).network)
        let res = try await store.install(release("9.9.9", sums: false), protecting: ["a2"])
        let left = Set(try fm.contentsOfDirectory(atPath: root.path))
        #expect(left.contains("a1") && left.contains("a2") && left.contains("9.9.9") && left.contains("current"))
        #expect(!left.contains("a0") && left.contains("a3"))
        #expect(res.pruned == ["a0"])
    }

    // MARK: remove

    private func makeRoot() throws -> (root: URL, a: URL, b: URL) {
        let root = tmp().appendingPathComponent("engine"), fm = FileManager.default
        let (a, b) = (root.appendingPathComponent("1.0.0"), root.appendingPathComponent("2.0.0"))
        for d in [a, b] { try fm.createDirectory(at: d, withIntermediateDirectories: true); try Data("x".utf8).write(to: d.appendingPathComponent("napple")) }
        try fm.createSymbolicLink(atPath: root.appendingPathComponent("current").path, withDestinationPath: "2.0.0")
        return (root, a, b)
    }

    private func build(_ id: String, folder: URL, bundled: Bool = false, active: Bool = false) -> EngineBuild {
        EngineBuild(id: id, version: id, folder: folder, isBundled: bundled, isActive: active, builtAt: nil)
    }

    @Test func removeDeletesAnInstalledVersionOnly() throws {
        let (root, a, b) = try makeRoot()
        let store = EngineStore(engineRoot: root)
        try store.remove(build("1.0.0", folder: a))
        #expect(!FileManager.default.fileExists(atPath: a.path))
        #expect(FileManager.default.fileExists(atPath: b.path))
        #expect(FileManager.default.fileExists(atPath: root.path))
    }

    @Test func removeRefusesWhatCurrentPointsAt() throws {
        let (root, _, b) = try makeRoot()
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("2.0.0", folder: b, active: true)) }
        #expect(FileManager.default.fileExists(atPath: b.path))
    }

    @Test func removeRefusesCurrentItself() throws {
        let (root, _, b) = try makeRoot()
        let cur = root.appendingPathComponent("current")
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("current", folder: cur)) }
        #expect(FileManager.default.fileExists(atPath: b.path))
        #expect((try? FileManager.default.destinationOfSymbolicLink(atPath: cur.path)) == "2.0.0")
    }

    @Test func removeRefusesTheBundledEngine() throws {
        let (root, a, _) = try makeRoot()
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build(EngineBuild.bundledID, folder: a, bundled: true)) }
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build(EngineBuild.bundledID, folder: a)) }
        #expect(FileManager.default.fileExists(atPath: a.path))
    }

    @Test func removeRefusesFoldersOutsideTheEngineRoot() throws {
        let (root, _, _) = try makeRoot()
        let outside = tmp().appendingPathComponent("9.9.9")
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("9.9.9", folder: outside)) }
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("x", folder: root)) }   // the root itself
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("1.0.0", folder: root.appendingPathComponent("../1.0.0/nested"))) }
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("1.0.0", folder: URL(fileURLWithPath: "/"))) }
        #expect(FileManager.default.fileExists(atPath: outside.path))
        #expect(FileManager.default.fileExists(atPath: root.path))
    }

    @Test func removeRefusesALinkedVersionFolderAndHiddenOnes() throws {
        let (root, _, _) = try makeRoot()
        let target = tmp().appendingPathComponent("real")
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
        let link = root.appendingPathComponent("3.0.0")
        try FileManager.default.createSymbolicLink(atPath: link.path, withDestinationPath: target.path)
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("3.0.0", folder: link)) }
        #expect(FileManager.default.fileExists(atPath: target.path))
        let staging = root.appendingPathComponent(".staging-x")
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build(".staging-x", folder: staging)) }
    }

    @Test func removeOfAMissingFolderIsAnError() throws {
        let (root, _, _) = try makeRoot()
        #expect(throws: EngineStoreError.self) { try EngineStore(engineRoot: root).remove(build("7.7.7", folder: root.appendingPathComponent("7.7.7"))) }
    }
}

@Suite struct ActiveKeysetTests {
    @Test func theNewestSetWinsAndDeletingFallsBackToTheFirst() throws {
        let root = tmp(), src = tmp(), m = KeysetManager(root: root)
        _ = try m.save(name: "Zulu", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        try await_(0.05)
        _ = try m.save(name: "Alpha", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        let sets = m.list()
        #expect(sets.map(\.name) == ["Alpha", "Zulu"])
        #expect(ActiveKeyset.newest(in: sets)?.name == "Alpha")
        #expect(ActiveKeyset.resolve(remembered: nil, in: sets) == "Alpha")
        #expect(ActiveKeyset.resolve(remembered: "Zulu", in: sets) == "Zulu")
        #expect(ActiveKeyset.resolve(remembered: "Gone", in: sets) == "Alpha")   // first remaining
        #expect(ActiveKeyset.resolve(remembered: "Gone", in: [sets[1]]) == "Zulu")
        #expect(ActiveKeyset.resolve(remembered: nil, in: []) == "")
    }

    private func await_(_ s: Double) throws { Thread.sleep(forTimeInterval: s) }
}
