import SwiftUI
import AppKit
import NappleStudioCore

enum Phase: Equatable { case idle, planning, ready, building, done, failed }

@MainActor
@Observable
final class AppModel {
    let keysets: KeysetManager
    let cli: NappleCLI?
    var sets: [Keyset] = []
    var selectedSetName: String = ""
    var files: [URL] = []
    var plan: PlanResult?
    var phase: Phase = .idle
    var errorText: String?
    var progress: ProgressEvent?
    var log: [String] = []
    var builtApp: URL?
    var compat: CompatEntry?
    var compatLoaded = false
    var showLegal = false
    var showSetup = false
    var installMessage: String?
    var legalAck: Bool { didSet { UserDefaults.standard.set(legalAck, forKey: "legalAck") } }
    private var buildTask: Task<Void, Never>?

    var notes: [String] = []
    var outputDir: URL {
        didSet { UserDefaults.standard.set(outputDir.path, forKey: "outputDir") }
    }
    var autoBuild = false
    var exitWhenDone = false
    static let defaultOutput = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/Napple", isDirectory: true)
    static let logsDir = KeysetManager.defaultRoot.appendingPathComponent("logs", isDirectory: true)
    private var logFile: FileHandle?

    /// The repo's matrix when running from a checkout, else the copy bundled in Resources.
    static var matrixURL: URL? {
        var d = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        for _ in 0..<8 {
            let c = d.appendingPathComponent("compat/matrix.json")
            if FileManager.default.fileExists(atPath: c.path) { return c }
            d = d.deletingLastPathComponent()
        }
        return Bundle.main.url(forResource: "matrix", withExtension: "json")
    }

    init() {
        let args = CommandLine.arguments
        func arg(_ n: String) -> String? { args.firstIndex(of: n).flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } }
        keysets = KeysetManager(root: arg("--support-dir").map { URL(fileURLWithPath: $0) } ?? KeysetManager.defaultRoot)
        cli = NappleCLI.locate()
        outputDir = arg("--output-dir").map { URL(fileURLWithPath: $0) }
            ?? UserDefaults.standard.string(forKey: "outputDir").map { URL(fileURLWithPath: $0) } ?? Self.defaultOutput
        autoBuild = args.contains("--debug-autobuild")
        legalAck = args.contains("--debug-ack") || UserDefaults.standard.bool(forKey: "legalAck")
        if let k = arg("--debug-keys") {
            _ = try? keysets.save(name: "Debug", keys: URL(fileURLWithPath: k), titleKeys: nil,
                              firmware: arg("--debug-firmware").map { URL(fileURLWithPath: $0) }, replace: true)
        }
        refreshSets()
        showSetup = sets.isEmpty
        if let d = arg("--debug-drop") { Task { await self.add(urls: [URL(fileURLWithPath: d)]) } }
    }

    var selectedSet: Keyset? { sets.first { $0.name == selectedSetName } ?? sets.first }

    func refreshSets() {
        sets = keysets.list()
        if !sets.contains(where: { $0.name == selectedSetName }) { selectedSetName = sets.first?.name ?? "" }
    }

    // MARK: plan
    func add(urls: [URL]) async {
        var all = files
        for u in urls where !all.contains(u) { all.append(u) }
        files = all
        await replan()
    }

    func reset() {
        buildTask?.cancel()
        files = []; plan = nil; phase = .idle; errorText = nil; progress = nil; log = []; builtApp = nil; compat = nil; notes = []; installMessage = nil
    }

    func replan() async {
        errorText = nil
        guard let set = selectedSet else { errorText = "Add keys first (Settings)."; showSetup = true; return }
        guard let cli else { errorText = CLIError.notFound.localizedDescription; return }
        phase = .planning
        let inputs = expand(files)
        do {
            let p = try await cli.plan(files: inputs, keys: set.keysURL)
            plan = p
            phase = .ready
            if let tid = p.base?.titleId {
                compat = Self.matrixURL.flatMap { CompatMatrix.load(from: $0) }?.entry(for: tid)
                compatLoaded = true
            }
            if autoBuild { autoBuild = false; startBuild() }
        } catch {
            plan = nil
            phase = .failed
            errorText = error.localizedDescription
            if exitWhenDone || CommandLine.arguments.contains("--debug-autobuild") { finishDebug(1) }
        }
    }

    private func finishDebug(_ code: Int32) {
        guard CommandLine.arguments.contains("--debug-autobuild") else { return }
        let delay = Double(CommandLine.arguments.firstIndex(of: "--debug-exit-delay").flatMap { CommandLine.arguments[safe: $0 + 1] } ?? "0") ?? 0
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { exit(code) }
    }

    /// Folders are expanded to the .nsp/.xci/.nsz files inside (one level deep recursion).
    private func expand(_ urls: [URL]) -> [URL] {
        var out: [URL] = []
        for u in urls {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir), isDir.boolValue,
               let e = FileManager.default.enumerator(at: u, includingPropertiesForKeys: nil) {
                for case let f as URL in e where ["nsp", "xci", "nsz"].contains(f.pathExtension.lowercased()) { out.append(f) }
            } else { out.append(u) }
        }
        return out
    }

    func toggle(updates: Bool, id: String) {
        guard var p = plan else { return }
        if updates, let i = p.updates.firstIndex(where: { $0.id == id }) { p.updates[i].selected.toggle() }
        if !updates, let i = p.dlc.firstIndex(where: { $0.id == id }) { p.dlc[i].selected.toggle() }
        plan = p
    }

    // MARK: build
    func startBuild() {
        guard let plan, let set = selectedSet, let cli else { return }
        guard legalAck else { showLegal = true; return }
        if let msg = diskPreflight(plan) { errorText = msg; phase = .failed; finishDebug(1); return }
        let out = outputDir
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        let req = BuildRequest(files: expand(files), keyset: set,
                               updates: plan.updates.filter(\.selected).map(\.version),
                               dlc: plan.dlc.filter(\.selected).map(\.titleId), output: out)
        phase = .building; errorText = nil; notes = []; installMessage = nil
        progress = ProgressEvent(stage: "preflight", percent: 0, message: "Checking keys and firmware"); log = []
        openLog()
        buildTask = Task {
            do {
                try await cli.check(keyset: set)
                let app = try await cli.build(req) { ev in
                    Task { @MainActor in self.handle(ev) }
                }
                try? await Task.sleep(nanoseconds: 200_000_000)
                builtApp = app ?? firstApp(in: out)
                phase = .done
                writeLog("DONE \(builtApp?.path ?? "?")")
                finishDebug(builtApp == nil ? 1 : 0)
            } catch {
                if Task.isCancelled { phase = .ready; errorText = "Build cancelled." } else { phase = .failed; errorText = error.localizedDescription }
                writeLog("FAILED \(error.localizedDescription)")
                finishDebug(1)
            }
        }
    }

    /// Rough need: selected packs + 1 GB working room (RomFS is written once, then cloned).
    func diskPreflight(_ plan: PlanResult) -> String? {
        let need = plan.totalSelectedBytes + 1_000_000_000
        var probe = outputDir
        while !FileManager.default.fileExists(atPath: probe.path), probe.path != "/" { probe.deleteLastPathComponent() }
        guard let free = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage,
              UInt64(free) < need else { return nil }
        return "Not enough disk space: about \(PlanResult.formatBytes(need)) needed, \(PlanResult.formatBytes(UInt64(free))) free. Free some space or choose another output folder in Settings."
    }

    private func openLog() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        let u = Self.logsDir.appendingPathComponent("build-\(f.string(from: Date())).log")
        FileManager.default.createFile(atPath: u.path, contents: nil, attributes: [.posixPermissions: 0o600])
        logFile = try? FileHandle(forWritingTo: u)
    }
    private static let logTime: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f }()
    private func writeLog(_ l: String) { logFile?.write(Data((Self.logTime.string(from: Date()) + " " + l + "\n").utf8)) }

    func cancelBuild() { buildTask?.cancel() }

    private func handle(_ ev: BuildEvent) {
        switch ev {
        case .progress(let p): progress = p; let l = "[\(p.stage)] \(Int(p.percent))% \(p.message)"; log.append(l); writeLog(l)
        case .result(let p): log.append("result: \(p)"); writeLog("RESULT \(p)")
        case .log(let l):
            log.append(l); writeLog(l); if log.count > 2000 { log.removeFirst(500) }
            if l.hasPrefix("note:") || l.hasPrefix("warning:") { notes.append(l) }
        }
    }

    private func firstApp(in dir: URL) -> URL? {
        (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil))?.first { $0.pathExtension == "app" }
    }

    // MARK: after build
    func launch() {
        guard let a = builtApp else { return }
        NSWorkspace.shared.openApplication(at: a, configuration: NSWorkspace.OpenConfiguration())
    }

    func reveal() { if let a = builtApp { NSWorkspace.shared.activateFileViewerSelecting([a]) } }

    func installToApplications() {
        guard let a = builtApp else { return }
        let dest = URL(fileURLWithPath: "/Applications").appendingPathComponent(a.lastPathComponent)
        do {
            if FileManager.default.fileExists(atPath: dest.path) { try FileManager.default.trashItem(at: dest, resultingItemURL: nil) }
            try FileManager.default.copyItem(at: a, to: dest)
            installMessage = "Installed to /Applications."
        } catch { installMessage = "Install failed: \(error.localizedDescription)" }
    }
}

extension Array { subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil } }
