import SwiftUI
import AppKit
import NappleStudioCore

/// `--debug-screen <name>`: puts the window into a fixed state with fake data so every screen can be photographed without a game or a device.
/// Only used by the screenshot script in tools/; nothing here runs in normal use.
extension AppModel {
    func applyDebugScreen(_ name: String) async {
        try? await Task.sleep(nanoseconds: 600_000_000)
        NSApp.activate()
        window?.makeKeyAndOrderFront(nil)
        if CommandLine.arguments.contains("--debug-light") { NSApp.appearance = NSAppearance(named: .aqua) }
        defer {
            let line = "debug-screen \(name): canBuild=\(canBuild) cli=\(cli != nil) set=\(selectedSet?.name ?? "-") disk=\(diskWarning ?? "ok") blocker=\(destinationBlocker ?? "none") key=\(window?.isKeyWindow ?? false)\n"
            FileHandle.standardError.write(Data(line.utf8))
        }
        let base = PlanItem(titleId: "010093801237C000", version: 0, versionLabel: "v2.1.0", size: 4_400_000_000, selected: true, path: "/x/base.nsp")
        let update = PlanItem(titleId: "010093801237C800", version: 65536, versionLabel: "v2.1.0", size: 310_000_000, selected: true, path: "/x/upd.nsp")
        let dlc = PlanItem(titleId: "010093801237D001", version: 0, versionLabel: "v0", size: 12_000_000, selected: false, path: "/x/dlc.nsp")
        func fakePlan(updates: [PlanItem] = [], dlc: [PlanItem] = [], warnings: [String] = []) -> PlanResult {
            PlanResult(base: base, updates: updates, dlc: dlc, warnings: warnings)
        }
        destination = .mac
        func ready(_ p: PlanResult) { files = [URL(fileURLWithPath: "/x/Metroid Dread [base].nsp")]; plan = p; phase = .ready; panelOpen = true }
        switch name {
        case "add": ready(fakePlan(updates: [update], dlc: [dlc]))
        case "add-ios":
            ready(fakePlan(updates: [update]))
            devices = [IOSDevice(id: "dev1", name: "Neto's iPhone 17 Pro", model: "iPhone18,1", osVersion: "26.1", connected: true, developerMode: true)]
            destination = .device("dev1")
        case "blockers": ready(fakePlan(warnings: ["Update 2.1.0 does not match this base game's version and was left out."]))
        case "reading": files = [URL(fileURLWithPath: "/x/Metroid Dread [base].nsp"), URL(fileURLWithPath: "/x/Metroid Dread [update].nsp")]; phase = .planning; panelOpen = true
        case "building", "building-closed":
            plan = fakePlan(); phase = .building; panelOpen = name == "building"
            buildStarted = Date().addingTimeInterval(-192)
            progress = ProgressEvent(stage: "rewrite", percent: 64, message: "rewrite: 11,650 of 18,204 functions")
            log = ["$ napple build --check Metroid Dread.nsp", "[preflight] 0% Checking keys and firmware", "plan: base 4.4 GB, update 2.1.0",
                   "warning: 3 shaders use an unsupported opcode, replay path enabled", "[rewrite] 41% 7,464 of 18,204 functions", "[rewrite] 64% 11,650 of 18,204 functions"]
        case "done":
            plan = fakePlan(); phase = .done; panelOpen = true
            builtApp = URL(fileURLWithPath: "/Users/neto/Applications/Napple/Metroid Dread.app"); builtBytes = 4_700_000_000; buildDuration = 408
            notes = ["note: 3 shaders use an unsupported opcode and run on the replay path."]
        case "failed":
            plan = fakePlan(); phase = .failed; panelOpen = true
            errorText = "The firmware in your key set is older than this game needs (20.1.0 required, 19.0.1 found). Add a newer firmware in Settings, then try again."
            log = ["$ napple build Metroid Dread.nsp", "preflight: keys ok", "error: firmware 19.0.1 is older than required 20.1.0", "build FAILED"]
        case "console": if let item = presentLibrary.first { playWithDebug(item) }
        case "selected": selectedLibraryItem = presentLibrary.first?.id
        case "search": searchText = "pokemon"
        case "legal": showLegal = true
        case "drop": dropTargeted = true
        default: break
        }
    }
}
