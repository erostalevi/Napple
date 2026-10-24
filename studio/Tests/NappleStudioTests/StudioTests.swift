import Foundation
import Testing
@testable import NappleStudioCore

func tmp() -> URL {
    let u = FileManager.default.temporaryDirectory.appendingPathComponent("napple-studio-\(UUID().uuidString)")
    try! FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
    return u
}

func goodKeys(in dir: URL, drop: String? = nil) -> URL {
    var names = KeysetManager.requiredKeyNames + ["header_key"]
    if let drop { names.removeAll { $0 == drop } }
    let u = dir.appendingPathComponent("prod.keys")
    try! (names.map { "\($0) = 00112233445566778899aabbccddeeff" }.joined(separator: "\n") + "\n").write(to: u, atomically: true, encoding: .utf8)
    return u
}

func fakeFirmwareDir(in dir: URL) -> URL {
    let f = dir.appendingPathComponent("fw"); try! FileManager.default.createDirectory(at: f, withIntermediateDirectories: true)
    try! Data([1]).write(to: f.appendingPathComponent("a.nca")); return f
}

@Suite struct KeysetTests {
    @Test func importListDeleteWithPermissions() throws {
        let root = tmp(), src = tmp()
        let m = KeysetManager(root: root)
        let s = try m.save(name: "One", keys: goodKeys(in: src), titleKeys: nil, firmware: fakeFirmwareDir(in: src))
        #expect(m.list().map(\.name) == ["One"])
        let attrs = try FileManager.default.attributesOfItem(atPath: s.keysURL.path)
        #expect((attrs[.posixPermissions] as? Int) == 0o600)
        #expect(s.firmwareURL != nil)
        try m.save(name: "Two", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        #expect(m.list().count == 2)           // multiple sets coexist
        try m.delete(s)
        #expect(m.list().map(\.name) == ["Two"])
    }

    @Test func rejectsMissingKeysBadFirmwareDuplicatesAndBadNames() throws {
        let root = tmp(), src = tmp()
        let m = KeysetManager(root: root)
        #expect(throws: KeysetError.self) { try m.save(name: "A", keys: goodKeys(in: src, drop: "master_key_00"), titleKeys: nil, firmware: nil) }
        let empty = src.appendingPathComponent("empty"); try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        #expect(throws: KeysetError.badFirmware) { try m.save(name: "A", keys: goodKeys(in: src), titleKeys: nil, firmware: empty) }
        #expect(throws: KeysetError.invalidName) { try m.save(name: "../x", keys: goodKeys(in: src), titleKeys: nil, firmware: nil) }
        try m.save(name: "A", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        #expect(throws: KeysetError.exists("A")) { try m.save(name: "A", keys: goodKeys(in: src), titleKeys: nil, firmware: nil) }
        try m.save(name: "A", keys: nil, titleKeys: nil, firmware: fakeFirmwareDir(in: src), replace: true)
        #expect(m.list()[0].firmwareURL != nil)
    }

    @Test func missingNamesNeverContainValues() throws {
        let missing = try KeysetManager.missingKeyNames(in: goodKeys(in: tmp(), drop: "titlekek_source"))
        #expect(missing == ["titlekek_source"])
        #expect(!missing.joined().contains("0011"))
    }
}

@Suite struct PlanTests {
    static let json = """
    {"base":{"title_id":"01006BB00C6F0000","version":0,"version_label":"v0 (release 0)","size":6274796032,"selected":true,"path":"/x.nsp"},
     "updates":[{"title_id":"01006BB00C6F0800","version":65536,"version_label":"v1.0.1","size":100,"selected":true,"path":"/u.nsp"}],
     "dlc":[{"title_id":"01006BB00C6F1001","version":0,"version_label":"v0","size":50,"selected":false,"path":"/d.nsp"}],
     "warnings":["w"],"future_field":1}
    """
    @Test func decodes() throws {
        let p = try PlanResult.decode(Data(Self.json.utf8))
        #expect(p.base?.titleId == "01006BB00C6F0000")
        #expect(p.updates[0].version == 65536 && p.dlc[0].selected == false)
        #expect(p.extraSelectedBytes == 100)
        #expect(p.totalSelectedBytes == 6274796132)
    }
    @Test func tolerantOfMissingLists() throws {
        let p = try PlanResult.decode(Data(#"{"base":null}"#.utf8))
        #expect(p.base == nil && p.updates.isEmpty)
    }
}

@Suite struct CompatTests {
    @Test func variants() {
        let a = CompatMatrix.parse(Data(#"[{"title_id":"01006bb00c6f0000","name":"LA","status":"Playable"}]"#.utf8))!
        #expect(a.entry(for: "01006BB00C6F0000")?.status == .playable)
        let b = CompatMatrix.parse(Data(#"{"games":{"AB":{"status":"boots","notes":"n"}}}"#.utf8))!
        #expect(b.entry(for: "ab")?.status.isAtLeastPartial == false)
        let c = CompatMatrix.parse(Data(#"{"AB":"weird"}"#.utf8))!
        #expect(c.entry(for: "AB")?.status == .unknown)
        #expect(CompatMatrix.parse(Data("nope".utf8)) == nil)
    }
}

@Suite struct MiscTests {
    @Test func humanize() {
        #expect(ErrorText.humanize("error: [disk-space] need 9 GB").contains("Free some disk space"))
        #expect(ErrorText.humanize("error: [cancelled] x") == "Build cancelled.")
        #expect(ErrorText.humanize("error: plain") == "plain")
    }
    @Test func deriveFromStages() {
        let m = CompatMatrix.parse(Data(#"{"games":[{"title_id":"AA","versions":[{"kind":"base","stages":{"ingame":{"status":"partial","reason":"r"},"ingest":{"status":"pass"}}}]}]}"#.utf8))!
        let e = m.entry(for: "AA")!
        #expect(e.status == .partial && e.stages.count == 2 && e.stages[0].name == "ingest")
    }
}

@Suite struct CLITests {
    func fake(_ body: String) -> NappleCLI {
        let u = tmp().appendingPathComponent("napple")
        try! ("#!/bin/sh\n" + body).write(to: u, atomically: true, encoding: .utf8)
        try! FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: u.path)
        return NappleCLI(executable: u)
    }
    @Test func parserLines() {
        #expect(ProgressParser.parse("PROGRESS decomp 42.5 Scanning main") == .progress(.init(stage: "decomp", percent: 42.5, message: "Scanning main")))
        #expect(ProgressParser.parse("PROGRESS x 250") == .progress(.init(stage: "x", percent: 100, message: "")))
        #expect(ProgressParser.parse("PROGRESS x nan") == .log("PROGRESS x nan"))
        #expect(ProgressParser.parse("RESULT /a/b.app") == .result("/a/b.app"))
        #expect(ProgressParser.parse("hello") == .log("hello"))
    }
    @Test func planViaFakeCLI() async throws {
        let cli = fake("cat <<'EOF'\n\(PlanTests.json)\nEOF\n")
        let p = try await cli.plan(files: [URL(fileURLWithPath: "/x")], keys: URL(fileURLWithPath: "/k"))
        #expect(p.updates.count == 1)
    }
    @Test func planErrorSurfaces() async {
        let cli = fake("echo 'error: bad keys' >&2; exit 1\n")
        await #expect(throws: CLIError.failed(code: 1, message: "bad keys")) {
            _ = try await cli.plan(files: [], keys: URL(fileURLWithPath: "/k"))
        }
    }
    @Test func buildNotAvailableAndProgress() async throws {
        let root = tmp(), src = tmp()
        let set = try KeysetManager(root: root).save(name: "S", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        let req = BuildRequest(files: [], keyset: set, updates: [], dlc: [], output: tmp())
        let old = fake("echo 'usage:\n  napple plan' >&2; exit 2\n")
        await #expect(throws: CLIError.notAvailable("build")) { _ = try await old.build(req) { _ in } }
        let ok = fake("echo 'PROGRESS rewrite 10 a'; echo 'PROGRESS pack 100 done'; echo 'RESULT /tmp/X.app'\n")
        let box = LockedBox<[String]>([])
        let app = try await ok.build(req) { ev in if case .progress(let p) = ev { box.append(p.stage) } }
        #expect(app?.path == "/tmp/X.app")
        #expect(box.value == ["rewrite", "pack"])
    }
    /// Lines must arrive while the build runs, not all at exit (the progress bar used to sit at 1% for the whole build).
    @Test func progressStreamsLive() async throws {
        let root = tmp(), src = tmp()
        let set = try KeysetManager(root: root).save(name: "S", keys: goodKeys(in: src), titleKeys: nil, firmware: nil)
        let req = BuildRequest(files: [], keyset: set, updates: [], dlc: [], output: tmp())
        let cli = fake("echo 'PROGRESS ingest 1 a'; echo 'note: x' >&2; sleep 1; echo 'PROGRESS done 100 b'\n")
        let t0 = Date(), first = LockedBox<Double?>(nil)
        _ = try await cli.build(req) { ev in if case .progress = ev, first.value == nil { first.value = Date().timeIntervalSince(t0) } }
        #expect((first.value ?? 99) < 0.8)
    }
}

@Suite struct KeyFinderTests {
    @Test func findsStandardNamesInSubfolders() throws {
        let root = tmp(), fm = FileManager.default
        let keysDir = root.appendingPathComponent("Switch/keys"); try fm.createDirectory(at: keysDir, withIntermediateDirectories: true)
        _ = goodKeys(in: keysDir)
        try "a = b\n".write(to: keysDir.appendingPathComponent("title.keys"), atomically: true, encoding: .utf8)
        for v in ["Firmware 17.0.0", "Firmware 19.0.1"] {
            let fw = root.appendingPathComponent("Switch/\(v)"); try fm.createDirectory(at: fw, withIntermediateDirectories: true)
            try Data([0]).write(to: fw.appendingPathComponent("a.nca"))
        }
        let r = KeyFinder.scan(root)
        #expect(r.keys?.lastPathComponent == "prod.keys")
        #expect(r.titleKeys?.lastPathComponent == "title.keys")
        #expect(r.firmware?.lastPathComponent == "Firmware 19.0.1")
    }
    @Test func skipsInvalidKeysAndReportsMissing() throws {
        let root = tmp()
        _ = goodKeys(in: root, drop: "master_key_00")
        let r = KeyFinder.scan(root)
        #expect(r.keys == nil && r.firmware == nil && r.titleKeys == nil)
    }
    @Test func firmwareFolderItself() throws {
        let root = tmp()
        try Data([0]).write(to: root.appendingPathComponent("x.nca"))
        #expect(KeyFinder.scan(root).firmware == root)
    }
}
