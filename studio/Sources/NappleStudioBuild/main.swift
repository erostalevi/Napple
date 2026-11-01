// napple-studio-build: Studio's build path without the window. It runs exactly what the Add Game / Build screens run (KeysetManager, NappleCLI.build with a
// BuildRequest), so tools/e2e builds games the way a user does. Usage:
//   napple-studio-build --out DIR [--keyset NAME] [--updates all|none|v,v] [--dlc all|none|id,id] [--target macos] [--name N] FILE...
// iPhone: --target ios --team TEAMID [--device ID] [--install] [--launch]  (Xcode makes the profile like Studio's Automatic signing; --install uses DeviceService)
// Prints PROGRESS lines as they come and, last, "RESULT <path to .app>"; exit status 0 only when an app was produced.
import Foundation
import NappleStudioCore

var args = Array(CommandLine.arguments.dropFirst())
var out: String?, keysetName: String?, updates = "all", dlc = "all", target = "macos", name: String?
var bundleArg: String?, team: String?, deviceId: String?, install = false, launchAfter = false
var files: [URL] = []
while !args.isEmpty {
    let a = args.removeFirst()
    func next() -> String { args.isEmpty ? "" : args.removeFirst() }
    switch a {
    case "--out": out = next()
    case "--keyset": keysetName = next()
    case "--updates": updates = next()
    case "--dlc": dlc = next()
    case "--target": target = next()
    case "--name": name = next()
    case "--team": team = next()
    case "--bundle-id": bundleArg = next()
    case "--device": deviceId = next()
    case "--install": install = true
    case "--launch": launchAfter = true
    default: files.append(URL(fileURLWithPath: a))
    }
}
files = InputExpander.expand(files).files   // folders become the game files inside, exactly as a drop on Studio does
guard let out, !files.isEmpty else {
    FileHandle.standardError.write(Data("usage: napple-studio-build --out DIR [--keyset NAME] [--updates all|none|v,v] [--dlc all|none|id,id] FILE...\n".utf8))
    exit(2)
}
guard let cli = NappleCLI.locate() else { FileHandle.standardError.write(Data("napple CLI not found (set NAPPLE_CLI)\n".utf8)); exit(2) }
let keysets = KeysetManager().list()
guard let keyset = keysetName.flatMap({ n in keysets.first { $0.name == n } }) ?? keysets.first else {
    FileHandle.standardError.write(Data("no keyset in \(KeysetManager.defaultRoot.path)\n".utf8)); exit(2)
}
let sem = DispatchSemaphore(value: 0)
Task {
    var code: Int32 = 1
    do {
        let plan = try await cli.plan(files: files, keys: keyset.keysURL, updates: updates, dlc: dlc)
        let ups = updates == "none" ? [] : plan.selectedUpdateVersions
        let dl = dlc == "none" ? [] : plan.selectedDLCIds
        var ios: IOSOptions?
        let bt = BuildTarget(rawValue: target) ?? .macos
        if bt.isIOS, !bt.isSimulator, let team {
            let signing = try await cli.listSigning()
            let titleId = plan.base?.titleId
            let bundle = bundleArg ?? BundleIDs.make(prefix: BundleIDs.defaultPrefix, titleId: titleId)
            let ents = IOSEntitlementPlan.make(account: .free, debug: false, profile: nil)
            print("PROGRESS provision 1 asking Xcode for a profile for \(bundle) (team \(team))"); fflush(stdout)
            let r = try await Provisioner().provision(teamId: team, bundleId: bundle, entitlements: ents,
                                                       work: FileManager.default.temporaryDirectory.appendingPathComponent("napple-provision-\(UUID().uuidString)"),
                                                       identities: signing.identities) { print($0); fflush(stdout) }
            guard let sha = r.identitySHA1 ?? signing.identities.first?.sha1 else { throw CLIError.failed(code: 1, message: "no certificate matches the profile") }
            ios = IOSOptions(identity: sha, profile: r.profile, bundleId: bundle)
            print("note: signing with \(r.details.name) (\(r.details.teamName)) identity \(sha)")
        }
        let req = BuildRequest(files: files, keyset: keyset, updates: ups, dlc: dl, target: target, output: URL(fileURLWithPath: out), name: name, ios: ios)
        let app = try await cli.build(req) { ev in
            switch ev {
            case .progress(let p): print("PROGRESS \(p.stage) \(Int(p.percent)) \(p.message)")
            case .log(let l): print(l)
            case .result(let r): print("RESULT \(r)")
            }
            fflush(stdout)
        }
        if install, let app, bt.isIOS {
            let svc = DeviceService()
            let (devs, note) = await svc.allDevices()
            if let note { print("note: \(note)") }
            guard let dev = devs.first(where: { deviceId == nil ? ($0.connected && !$0.simulator) : $0.id == deviceId }) else { throw CLIError.failed(code: 1, message: "no device") }
            print("PROGRESS install 0 installing on \(dev.summary)"); fflush(stdout)
            try await svc.install(app: app, on: dev) { print($0); fflush(stdout) }
            if launchAfter, let b = NappleCLI.bundleId(of: app) { try await svc.launch(bundleId: b, on: dev) }
            print("INSTALLED \(dev.name)")
        }
        if let app { print("RESULT \(app.path)"); code = 0 }
    } catch { FileHandle.standardError.write(Data("error: \(error.localizedDescription)\n".utf8)) }
    exit(code)
}
dispatchMain()
