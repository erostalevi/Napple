import Testing
import Foundation
@testable import NappleStudioCore

private func sysTmp() -> URL {
    let u = FileManager.default.temporaryDirectory.appendingPathComponent("studio-sys-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
    return u
}
/// A database file in the runtime's layout with the given nicknames (entry CRCs are the runtime's business; Studio checks the header only).
private func makeDB(_ names: [String]) -> Data {
    var body = [UInt8]()
    for n in names {
        var e = [UInt8](repeating: 0, count: MiiStore.entrySize)
        for (k, c) in n.utf16.prefix(10).enumerated() { e[0x1c + 2 * k] = UInt8(c & 0xff); e[0x1c + 2 * k + 1] = UInt8(c >> 8) }
        body += e
    }
    var h = [UInt8](repeating: 0, count: 16)
    h[0] = 0x4e; h[1] = 0x46; h[2] = 0x44; h[3] = 0x42; h[4] = 1; h[5] = UInt8(names.count)
    let crc = MiiStore.crc16(body[...])
    h[6] = UInt8(crc & 0xff); h[7] = UInt8(crc >> 8); h[8] = 1
    return Data(h + body)
}

@Suite struct SystemDataTests {
    @Test func supportDirFollowsTheEnvironment() {
        #expect(SystemData.supportDir(env: ["NAPPLE_SUPPORT_DIR": "/x/y"]).path == "/x/y")
        #expect(SystemData.supportDir(env: [:]).path.hasSuffix("Library/Application Support/Napple"))
    }
    @Test func configRoundTripUsesTheRuntimeKeys() throws {
        let d = sysTmp(); defer { try? FileManager.default.removeItem(at: d) }
        var c = SystemConfig(); c.amiiboDatabase = "https://example.invalid/amiibo.json"; c.amiiboKey = "/k/key_retail.bin"; c.onlineProvider = "/p/prov.dylib"
        try c.save(support: d)
        let raw = try JSONSerialization.jsonObject(with: Data(contentsOf: SystemConfig.url(support: d))) as? [String: String]
        #expect(raw?["amiibo_database"] == c.amiiboDatabase)
        #expect(raw?["online_provider"] == "/p/prov.dylib")
        #expect(raw?["amiibo_store"] == nil)          // empty values are not written
        #expect(SystemConfig.load(support: d) == c)
        #expect(SystemConfig.load(support: sysTmp()) == SystemConfig())
    }
    @Test func onlineProviderChecksThePath() {
        var c = SystemConfig(); #expect(c.onlineProviderUsable)
        c.onlineProvider = "/nonexistent/provider.dylib"; #expect(!c.onlineProviderUsable)
    }
    @Test func miiListDeleteExportRestore() throws {
        let d = sysTmp(); defer { try? FileManager.default.removeItem(at: d) }
        let s = MiiStore(folder: d)
        #expect(!s.exists && s.entries().isEmpty)
        try makeDB(["Neto", "Ana", "Bob"]).write(to: s.database)
        #expect(s.entries().map(\.name) == ["Neto", "Ana", "Bob"])
        let backup = try s.backup()
        try s.delete(index: 1)
        #expect(s.entries().map(\.name) == ["Neto", "Bob"])
        #expect(FileManager.default.fileExists(atPath: s.database.appendingPathExtension("bak").path))
        try s.restore(from: backup)
        #expect(s.entries().count == 3)
        let out = sysTmp(); defer { try? FileManager.default.removeItem(at: out) }
        let exported = try s.export(to: out)
        #expect(exported.lastPathComponent == "MiiDatabase.dat")
        #expect(throws: (any Error).self) { try s.restore(from: out.appendingPathComponent("missing.dat")) }
    }
    @Test func lastMiiStays() throws {
        let d = sysTmp(); defer { try? FileManager.default.removeItem(at: d) }
        let s = MiiStore(folder: d); try makeDB(["Only"]).write(to: s.database)
        #expect(throws: (any Error).self) { try s.delete(index: 0) }
    }
    @Test func gameRunStreamsTheLog() async throws {
        let d = sysTmp(); defer { try? FileManager.default.removeItem(at: d) }
        let app = d.appendingPathComponent("T.app/Contents")
        try FileManager.default.createDirectory(at: app.appendingPathComponent("MacOS"), withIntermediateDirectories: true)
        let exe = app.appendingPathComponent("MacOS/T")
        try "#!/bin/sh\necho hello-out\necho hello-err >&2\nexit 3\n".write(to: exe, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: exe.path)
        try (["CFBundleExecutable": "T"] as NSDictionary).write(to: app.appendingPathComponent("Info.plist"))
        let lines = LockedBox<[String]>([]), code = LockedBox<Int32>(-1)
        let run = GameRun(app: d.appendingPathComponent("T.app"))
        try run.start(onLine: { lines.append($0) }, onExit: { code.value = $0 })
        for _ in 0..<100 where code.value < 0 { try await Task.sleep(for: .milliseconds(50)) }
        try await Task.sleep(for: .milliseconds(200))
        #expect(code.value == 3)
        #expect(Set(lines.value) == ["hello-out", "hello-err"])
        #expect(GameRun(app: d).isRunning == false)
        #expect(throws: (any Error).self) { try GameRun(app: d).start(onLine: { _ in }, onExit: { _ in }) }
    }
}
