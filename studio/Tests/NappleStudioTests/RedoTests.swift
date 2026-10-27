import Testing
import Foundation
@testable import NappleStudioCore

private func profile(days: Double, team: String = "TEAM123456", name: String = "Someone", memory: Bool = false) -> ProfileDetails {
    let c = Date(timeIntervalSince1970: 1_700_000_000)
    return ProfileDetails(name: "p", teamId: team, teamName: name, appId: "\(team).com.napple.x", created: c,
                          expires: c.addingTimeInterval(days * 86_400),
                          entitlements: memory ? [ProfileDetails.increasedMemory: true] : [:])
}

@Suite struct AccountTests {
    @Test func xcodeTeamListWins() {
        let teams = [XcodeTeam(teamId: "TEAM123456", name: "x (Personal Team)", type: "Personal Team", isFree: true)]
        #expect(AccountDetector.classify(profile: profile(days: 365), teams: teams) == .free)
        let paid = [XcodeTeam(teamId: "TEAM123456", name: "Acme", type: "Company/Organization", isFree: false)]
        #expect(AccountDetector.classify(profile: profile(days: 7), teams: paid) == .paid)
    }
    @Test func profileEvidenceWithoutXcodeList() {
        #expect(AccountDetector.classify(profile: profile(days: 7), teams: []) == .free)
        #expect(AccountDetector.classify(profile: profile(days: 365), teams: []) == .paid)
        #expect(AccountDetector.classify(profile: profile(days: 30, memory: true), teams: []) == .paid)
        #expect(AccountDetector.classify(profile: profile(days: 30, name: "A (Personal Team)"), teams: []) == .free)
        #expect(AccountDetector.classify(profile: profile(days: 30), teams: []) == .unknown)
        #expect(AccountDetector.classify(profile: nil, teams: []) == .unknown)
    }
    @Test func xcodePrefsParse() {
        let prefs: [String: Any] = ["acct": [["teamID": "AAA", "teamName": "N", "teamType": "Personal Team", "isFreeProvisioningTeam": true],
                                             ["teamID": "BBB", "teamName": "Corp", "teamType": "Company/Organization", "isFreeProvisioningTeam": false]]]
        let t = XcodeTeam.parse(prefs)
        #expect(t.map(\.teamId) == ["AAA", "BBB"])
        #expect(t[0].isFree && !t[1].isFree)
    }
    @Test func profilePlistParse() {
        let p = ProfileDetails.parse(plist: ["Name": "n", "TeamName": "T", "TeamIdentifier": ["ABC"], "CreationDate": Date(timeIntervalSince1970: 0),
            "ExpirationDate": Date(timeIntervalSince1970: 86_400 * 7), "ProvisionedDevices": ["a", "b"],
            "Entitlements": ["application-identifier": "ABC.com.x", ProfileDetails.increasedMemory: true, "x": "str"]])
        #expect(p.teamId == "ABC" && p.deviceCount == 2 && p.grantsIncreasedMemory && p.lifetimeDays == 7 && p.appId == "ABC.com.x")
        #expect(p.entitlements["x"] == nil)
    }
}

@Suite struct MemoryEntitlementTests {
    @Test func paidReleaseGetsMemoryOnly() {
        let p = IOSEntitlementPlan.make(account: .paid, debug: false)
        #expect(p.includesMemory && p.entitlements == [ProfileDetails.increasedMemory: true] && !p.needsCapability)
    }
    @Test func paidDebugAlsoGetsDebuggable() {
        let p = IOSEntitlementPlan.make(account: .paid, debug: true)
        #expect(p.entitlements[ProfileDetails.increasedDebuggableMemory] == true && p.entitlements[ProfileDetails.increasedMemory] == true)
    }
    @Test func paidWithProfileMissingCapabilityIsFlagged() {
        let p = IOSEntitlementPlan.make(account: .paid, debug: false, profile: profile(days: 365))
        #expect(p.needsCapability && p.explanation.contains("automatic signing"))
        #expect(!IOSEntitlementPlan.make(account: .paid, debug: false, profile: profile(days: 365, memory: true)).needsCapability)
    }
    @Test func freeAndUnknownAddNothing() {
        for k in [AccountKind.free, .unknown] {
            let p = IOSEntitlementPlan.make(account: k, debug: true)
            #expect(!p.includesMemory && p.entitlements.isEmpty && !p.explanation.isEmpty)
            let plist = try? PropertyListSerialization.propertyList(from: p.plist(), format: nil) as? [String: Any]
            #expect(plist?.isEmpty == true)
        }
    }
    @Test func plistCarriesKeys() throws {
        let p = IOSEntitlementPlan.make(account: .paid, debug: false)
        let d = try #require(try PropertyListSerialization.propertyList(from: p.plist(base: ["get-task-allow": false]), format: nil) as? [String: Any])
        #expect(d[ProfileDetails.increasedMemory] as? Bool == true && d["get-task-allow"] as? Bool == false)
    }
}

@Suite struct ProvisionerTests {
    func tmp() -> URL { FileManager.default.temporaryDirectory.appendingPathComponent("prov-\(UUID().uuidString)") }
    @Test func stubProjectCarriesTeamBundleAndEntitlements() async throws {
        let keep = ProcessInfo.processInfo.environment["NAPPLE_STUB_OUT"].map { URL(fileURLWithPath: $0) }   // keeps the generated project for a manual xcodebuild check
        let d = keep ?? tmp(); defer { if keep == nil { try? FileManager.default.removeItem(at: d) } }
        try Provisioner.writeStubProject(in: d, teamId: "TEAM123456", bundleId: "com.napple.game.0100152000022000", entitlements: .make(account: .paid, debug: false))
        let pbx = try String(contentsOf: d.appendingPathComponent("Stub.xcodeproj/project.pbxproj"), encoding: .utf8)
        #expect(pbx.contains("DEVELOPMENT_TEAM = \"TEAM123456\"") && pbx.contains("PRODUCT_BUNDLE_IDENTIFIER = \"com.napple.game.0100152000022000\"") && pbx.contains("CODE_SIGN_STYLE = Automatic"))
        let ents = try #require(try PropertyListSerialization.propertyList(from: Data(contentsOf: d.appendingPathComponent("Stub/Stub.entitlements")), format: nil) as? [String: Any])
        #expect(ents[ProfileDetails.increasedMemory] as? Bool == true)
        try Provisioner.writeStubProject(in: d, teamId: "T", bundleId: "b", entitlements: .make(account: .free, debug: true))
        let free = try #require(try PropertyListSerialization.propertyList(from: Data(contentsOf: d.appendingPathComponent("Stub/Stub.entitlements")), format: nil) as? [String: Any])
        #expect(free.isEmpty)
        #expect(Provisioner.commandLine(projectDir: d).contains("-allowProvisioningUpdates"))
        // NAPPLE_XCODE_CHECK=1: let the real xcodebuild parse the generated project (read-only; no provisioning)
        if ProcessInfo.processInfo.environment["NAPPLE_XCODE_CHECK"] == "1" {
            let out = try await ToolRunner.system.xcrun(["xcodebuild", "-project", d.appendingPathComponent("Stub.xcodeproj").path, "-target", "Stub", "-showBuildSettings"])
            #expect(out.stdout.contains("PRODUCT_BUNDLE_IDENTIFIER = b") && out.stdout.contains("DEVELOPMENT_TEAM = T") && out.stdout.contains("CODE_SIGN_STYLE = Automatic"))
        }
    }
    @Test func newestCoveringProfileWins() throws {
        let d = tmp(); defer { try? FileManager.default.removeItem(at: d) }
        try FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        for n in ["old", "new", "other", "wild"] { try Data().write(to: d.appendingPathComponent("\(n).mobileprovision")) }
        let t0 = Date(timeIntervalSince1970: 1_000)
        func fake(_ u: URL) -> ProfileDetails? {
            switch u.deletingPathExtension().lastPathComponent {
            case "old": return ProfileDetails(name: "old", teamId: "T", appId: "T.com.x.a", created: t0)
            case "new": return ProfileDetails(name: "new", teamId: "T", appId: "T.com.x.a", created: t0.addingTimeInterval(60))
            case "other": return ProfileDetails(name: "other", teamId: "T", appId: "T.com.x.b", created: t0.addingTimeInterval(600))
            default: return ProfileDetails(name: "wild", teamId: "Z", appId: "Z.*", created: t0.addingTimeInterval(900))
            }
        }
        #expect(Provisioner.newestProfile(in: d, teamId: "T", bundleId: "com.x.a", read: fake)?.details.name == "new")
        #expect(Provisioner.newestProfile(in: d, teamId: "T", bundleId: "com.x.c", read: fake) == nil)
    }
    @Test func certificateFingerprints() {
        var p = ProfileDetails(); p.certificates = [Data("abc".utf8)]
        #expect(p.certificateSHA1s == ["A9993E364706816ABA3E25717850C26C9CD0D89D"])
        let parsed = ProfileDetails.parse(plist: ["DeveloperCertificates": [Data("abc".utf8)], "Entitlements": [:]])
        #expect(parsed.certificateSHA1s.count == 1)
    }
    @Test func provisionWithFakeXcode() async throws {
        let profiles = tmp(), work = tmp(); defer { try? FileManager.default.removeItem(at: profiles); try? FileManager.default.removeItem(at: work) }
        try FileManager.default.createDirectory(at: profiles, withIntermediateDirectories: true)
        let log = LockedBox<[String]>([])
        let tools = ToolRunner { _, args, _ in
            log.append(args.joined(separator: " "))
            try Data().write(to: profiles.appendingPathComponent("made.mobileprovision"))   // what Xcode would download
            return ToolOutput(stdout: "", stderr: "", status: 0)
        }
        // newestProfile reads real files through ProfileDetails.read (security cms); the fake profile is empty, so this exercises the "no profile" error path
        let p = Provisioner(tools: tools, profilesDir: profiles)
        await #expect(throws: CLIError.self) {
            _ = try await p.provision(teamId: "T", bundleId: "com.x.a", entitlements: .make(account: .free, debug: false), work: work, identities: [])
        }
        #expect(log.value.first?.contains("xcodebuild -project") == true && log.value.first?.contains("-allowProvisioningDeviceRegistration") == true)
        #expect(FileManager.default.fileExists(atPath: work.appendingPathComponent("Stub.xcodeproj/project.pbxproj").path))
    }
    @Test func explainAdvice() {
        #expect(Provisioner.explain("error: No Account for Team \"X\"").contains("Settings > Accounts"))
        #expect(Provisioner.explain("No devices registered for this team").contains("Connect the iPhone"))
        #expect(Provisioner.explain("odd").hasPrefix("odd"))
    }
}

@Suite struct DeviceTests {
    static let devicectl = """
    {"result":{"devices":[
     {"identifier":"AAA","deviceProperties":{"name":"iPhone","osVersionNumber":"26.1","developerModeStatus":"enabled"},
      "hardwareProperties":{"platform":"iOS","marketingName":"iPhone 16 Pro Max","reality":"physical"},"connectionProperties":{"tunnelState":"connected"}},
     {"identifier":"BBB","deviceProperties":{"name":"Old","osVersionNumber":"17.2","developerModeStatus":"disabled"},
      "hardwareProperties":{"platform":"iOS","marketingName":"iPhone 12"},"connectionProperties":{"tunnelState":"disconnected"}},
     {"identifier":"WATCH","hardwareProperties":{"platform":"watchOS"}}]}}
    """
    @Test func parsesPhysical() {
        let d = DeviceList.parse(Data(Self.devicectl.utf8))
        #expect(d.count == 2 && d[0].connected && d[0].supported && !d[1].supported && !d[1].developerMode)
        #expect(d[1].summary.contains("not connected"))
    }
    @Test func parsesSimulators() {
        let j = #"{"devices":{"com.apple.CoreSimulator.SimRuntime.iOS-26-1":[{"udid":"U1","name":"iPhone 17","state":"Booted"},{"udid":"U2","name":"x","state":"Shutdown"}],"com.apple.CoreSimulator.SimRuntime.tvOS-26-0":[{"udid":"T","name":"tv","state":"Booted"}]}}"#
        let s = SimulatorList.parse(Data(j.utf8))
        #expect(s.count == 1 && s[0].simulator && s[0].osVersion == "26.1" && s[0].supported)
    }
    @Test func installAndLaunchCommands() async throws {
        let log = LockedBox<[String]>([])
        let svc = DeviceService(tools: ToolRunner { exe, args, _ in log.append(args.joined(separator: " ")); return ToolOutput(stdout: "", stderr: "", status: 0) })
        var dev = IOSDevice(id: "AAA", name: "iPhone", model: "m", osVersion: "26.1", connected: true, developerMode: true)
        try await svc.install(app: URL(fileURLWithPath: "/x/G.app"), on: dev)
        try await svc.launch(bundleId: "com.napple.g", on: dev)
        dev.simulator = true; dev.id = "U1"
        try await svc.install(app: URL(fileURLWithPath: "/x/G.app"), on: dev)
        try await svc.launch(bundleId: "com.napple.g", on: dev)
        #expect(log.value == ["devicectl device install app --device AAA /x/G.app", "devicectl device process launch --device AAA --terminate-existing com.napple.g",
                              "simctl install U1 /x/G.app", "simctl launch --terminate-running-process U1 com.napple.g"])
        #expect(DeviceService.commandLines(app: URL(fileURLWithPath: "/x/G.app"), bundleId: "b", on: dev).count == 2)
    }
    @Test func mockedDiscovery() async {
        let svc = DeviceService(tools: ToolRunner { _, args, _ in
            if args.contains("--json-output"), let i = args.firstIndex(of: "--json-output") { try Data(DeviceTests.devicectl.utf8).write(to: URL(fileURLWithPath: args[i + 1])) }
            return ToolOutput(stdout: args.contains("-j") ? #"{"devices":{}}"# : "", stderr: "", status: 0)
        })
        let r = await svc.allDevices()
        #expect(r.devices.count == 2 && r.note == nil)
    }
    @Test func discoveryFailureIsReported() async {
        let svc = DeviceService(tools: ToolRunner { _, _, _ in throw CLIError.failed(code: 1, message: "boom") })
        let r = await svc.allDevices()
        #expect(r.devices.isEmpty && r.note?.contains("boom") == true)
    }
}

@Suite struct BundleIDTests {
    @Test func perTitle() {
        #expect(BundleIDs.make(prefix: "com.napple.game", titleId: "0100152000022000") == "com.napple.game.0100152000022000")
        #expect(BundleIDs.make(prefix: "com.napple.game", titleId: "01006BB00C6F0000") != BundleIDs.make(prefix: "com.napple.game", titleId: "0100152000022000"))
        #expect(BundleIDs.make(prefix: "My Prefix!", titleId: "0100152000022000") == "myprefix.0100152000022000")
        #expect(BundleIDs.make(prefix: "", titleId: nil) == "com.napple.game")
    }
    @Test func profilePinsOrWildcards() {
        #expect(BundleIDs.make(prefix: "com.napple.game", titleId: "0100152000022000", profileAppId: "TEAM.com.napple.mk8d") == "com.napple.mk8d")
        #expect(BundleIDs.make(prefix: "com.napple.game", titleId: "0100152000022000", profileAppId: "TEAM.*") == "com.napple.game.0100152000022000")
        #expect(BundleIDs.make(prefix: "com.napple.game", titleId: "0100152000022000", profileAppId: "TEAM.com.napple.*") == "com.napple.game.0100152000022000")
    }
}

