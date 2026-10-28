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

@Suite struct IOSTests {
    static let devices = """
    {"info":{"outcome":"success"},"result":{"devices":[
      {"identifier":"AAAA-1111","deviceProperties":{"name":"Neto's iPhone","osVersionNumber":"26.0.1","developerModeStatus":"enabled"},
       "hardwareProperties":{"marketingName":"iPhone 17 Pro","platform":"iOS","deviceType":"iPhone","udid":"00008150-001"},"connectionProperties":{"tunnelState":"connected"}},
      {"identifier":"BBBB-2222","deviceProperties":{"name":"Old iPad","osVersionNumber":"18.4","developerModeStatus":"disabled"},
       "hardwareProperties":{"marketingName":"iPad Air","platform":"iOS","deviceType":"iPad"},"connectionProperties":{"tunnelState":"disconnected"}},
      {"identifier":"CCCC-3333","deviceProperties":{"name":"Watch","osVersionNumber":"11"},"hardwareProperties":{"platform":"watchOS","deviceType":"appleWatch"}}
    ]}}
    """

    @Test func parsesDevicectlListAndFlagsUnsupportedDevices() throws {
        let d = DeviceList.parse(Data(Self.devices.utf8))
        #expect(d.map(\.id) == ["AAAA-1111", "BBBB-2222"])          // the watch is not a target
        #expect(d[0].connected && d[0].developerMode && d[0].supported)
        #expect(!d[1].connected && !d[1].developerMode && !d[1].supported)
        #expect(d[1].summary.contains("not connected"))
        #expect(DeviceList.parse(Data("{}".utf8)).isEmpty && DeviceList.parse(Data("junk".utf8)).isEmpty)
    }

    @Test func decodesSigningListAndMatchesProfilesToBundleIds() throws {
        let json = """
        {"identities":[{"sha1":"ABCDEF0123456789ABCDEF0123456789ABCDEF01","name":"Apple Development: me@example.com (TEAM123456)"}],
         "profiles":[{"path":"/p/a.mobileprovision","name":"iOS Team Provisioning Profile: com.me.napple","team":"TEAM123456","app_id":"TEAM123456.com.me.napple","expires":"2026-11-01T00:00:00Z","devices":1},
                     {"path":"/p/b.mobileprovision","name":"Wildcard","team":"TEAM123456","app_id":"TEAM123456.*","expires":"2027-01-01T00:00:00Z","devices":3}]}
        """
        let s = try SigningInfo.decode(Data(json.utf8))
        #expect(s.identities.count == 1 && s.profiles.count == 2)
        let (explicit, wild) = (s.profiles[0], s.profiles[1])
        #expect(explicit.covers(bundleId: "com.me.napple") && !explicit.covers(bundleId: "com.me.other"))
        #expect(wild.covers(bundleId: "com.napple.game.0100000000010000"))
        #expect(explicit.explicitBundleId == "com.me.napple" && wild.explicitBundleId == nil)
    }

    @Test func buildArgumentsCarrySigningOptions() {
        #expect(BuildTarget.allCases.map(\.rawValue) == ["macos", "ios", "iphone", "ipad", "ios-sim"])
        #expect(BuildTarget.ios.isIOS && !BuildTarget.macos.isIOS)
        #expect(ErrorText.humanize("error: [install] devicectl failed").contains("Developer Mode"))
    }
}

@Suite struct EngineInstallerTests {
    func bundle(in dir: URL) -> URL {
        let r = dir.appendingPathComponent("Resources"); try! FileManager.default.createDirectory(at: r, withIntermediateDirectories: true)
        for f in EngineInstaller.requiredFiles {
            let u = r.appendingPathComponent(f); try! "#!/bin/sh\n".write(to: u, atomically: true, encoding: .utf8)
            try! FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: u.path)
        }
        return r
    }

    /// Every local build is stamped with the same version, so a stale engine of an older build must be replaced when the bundled binaries differ.
    @Test func replacesAStaleEngineOfTheSameVersion() throws {
        let root = tmp(), home = tmp()
        let res = bundle(in: root)
        var inst = EngineInstaller(resources: res, support: home.appendingPathComponent("Support"), home: home)
        inst.setLaunchctl = false
        try inst.install(version: "1-1")
        #expect(inst.isUpToDate(version: "1-1"))
        // a newer Studio build with the same version string but a different engine
        let napple = res.appendingPathComponent("napple")
        let old = try Data(contentsOf: napple)
        try (old + Data("-rebuilt".utf8)).write(to: napple)
        #expect(!inst.isUpToDate(version: "1-1"))
        let r = try inst.install(version: "1-1")
        #expect(r.installed && inst.isUpToDate(version: "1-1"))
        #expect(try Data(contentsOf: inst.cli) == (old + Data("-rebuilt".utf8)))
    }

    /// The build screen's engine picker lists Studio's own engine and every installed version, newest first; several versions stay installed.
    @Test func listsEveryEngineBuild() throws {
        let root = tmp(), home = tmp()
        let res = bundle(in: root)
        var inst = EngineInstaller(resources: res, support: home.appendingPathComponent("Support"), home: home)
        inst.setLaunchctl = false
        #expect(inst.listEngines().map(\.id) == [EngineBuild.bundledID])
        try inst.install(version: "1-1")
        try inst.install(version: "1-2")
        try inst.install(version: "1-3")
        let l = inst.listEngines(bundleVersion: "1-3")
        #expect(l.first?.isBundled == true && l.first?.version == "1-3")
        #expect(Set(l.dropFirst().map(\.id)) == ["1-1", "1-2", "1-3"])
        #expect(l.filter(\.isActive).map(\.id) == ["1-3"])
        #expect(FileManager.default.isExecutableFile(atPath: l[1].cliURL.path))
    }

    @Test func installsLinksAndIsIdempotent() throws {
        let root = tmp(), home = tmp()
        var inst = EngineInstaller(resources: bundle(in: root), support: home.appendingPathComponent("Support"), home: home)
        inst.setLaunchctl = false
        #expect(!inst.isUpToDate(version: "1-1"))
        let r = try inst.install(version: "1-1")
        #expect(r.installed && r.notes.isEmpty)
        #expect(inst.isUpToDate(version: "1-1"))
        #expect(FileManager.default.isExecutableFile(atPath: inst.cli.path))
        #expect(try FileManager.default.destinationOfSymbolicLink(atPath: inst.binLink.path) == inst.cli.path)
        let env = try String(contentsOf: inst.envScript, encoding: .utf8)
        #expect(env.contains("NAPPLE_CLI=") && env.contains("NAPPLE_HOME="))
        let profile = try String(contentsOf: inst.zprofile, encoding: .utf8)
        #expect(profile.components(separatedBy: EngineInstaller.profileMarkerBegin).count == 2)

        let again = try inst.install(version: "1-1")
        #expect(!again.installed)
        let up = try inst.install(version: "1-2")
        #expect(up.installed && inst.isUpToDate(version: "1-2"))
        let profile2 = try String(contentsOf: inst.zprofile, encoding: .utf8)
        #expect(profile2.components(separatedBy: EngineInstaller.profileMarkerBegin).count == 2)
    }

    @Test func keepsUserProfileAndForeignBinary() throws {
        let root = tmp(), home = tmp()
        try "export FOO=1".write(to: home.appendingPathComponent(".zprofile"), atomically: true, encoding: .utf8)
        try FileManager.default.createDirectory(at: home.appendingPathComponent(".local/bin"), withIntermediateDirectories: true)
        try "mine".write(to: home.appendingPathComponent(".local/bin/napple"), atomically: true, encoding: .utf8)
        var inst = EngineInstaller(resources: bundle(in: root), support: home.appendingPathComponent("Support"), home: home)
        inst.setLaunchctl = false
        let r = try inst.install(version: "1-1")
        #expect(r.notes.count == 1)
        #expect(try String(contentsOf: inst.binLink, encoding: .utf8) == "mine")
        #expect(try String(contentsOf: inst.zprofile, encoding: .utf8).hasPrefix("export FOO=1\n"))
    }

    @Test func missingEngineThrows() {
        let inst = EngineInstaller(resources: tmp(), support: tmp(), home: tmp())
        #expect(throws: CLIError.self) { try inst.install(version: "1") }
    }
}
