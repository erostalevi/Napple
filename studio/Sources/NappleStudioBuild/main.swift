// napple-studio-build: Studio's build path without the window. It runs exactly what the Add Game / Build screens run (KeysetManager, NappleCLI.build with a
// BuildRequest), so tools/e2e builds games the way a user does. Usage:
//   napple-studio-build --out DIR [--keyset NAME] [--updates all|none|v,v] [--dlc all|none|id,id] [--target macos] [--name N] FILE...
// Prints PROGRESS lines as they come and, last, "RESULT <path to .app>"; exit status 0 only when an app was produced.
import Foundation
import NappleStudioCore

var args = Array(CommandLine.arguments.dropFirst())
var out: String?, keysetName: String?, updates = "all", dlc = "all", target = "macos", name: String?
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
        let req = BuildRequest(files: files, keyset: keyset, updates: ups, dlc: dl, target: target, output: URL(fileURLWithPath: out), name: name)
        let app = try await cli.build(req) { ev in
            switch ev {
            case .progress(let p): print("PROGRESS \(p.stage) \(Int(p.percent)) \(p.message)")
            case .log(let l): print(l)
            case .result(let r): print("RESULT \(r)")
            }
            fflush(stdout)
        }
        if let app { print("RESULT \(app.path)"); code = 0 }
    } catch { FileHandle.standardError.write(Data("error: \(error.localizedDescription)\n".utf8)) }
    exit(code)
}
dispatchMain()
