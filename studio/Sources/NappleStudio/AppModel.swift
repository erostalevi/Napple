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
    var engineMessage: String?
    // iOS / iPadOS targets (docs/ios.md)
    var target: BuildTarget = BuildTarget(rawValue: UserDefaults.standard.string(forKey: "target") ?? "") ?? .macos {
        didSet { UserDefaults.standard.set(target.rawValue, forKey: "target"); if target.isIOS { Task { await refreshIOS() } } }
    }
    var signing: SigningInfo?
    var devices: [IOSDevice] = []
    var iosIdentity: String = UserDefaults.standard.string(forKey: "iosIdentity") ?? "" { didSet { UserDefaults.standard.set(iosIdentity, forKey: "iosIdentity") } }
    var iosProfilePath: String = UserDefaults.standard.string(forKey: "iosProfile") ?? "" { didSet { UserDefaults.standard.set(iosProfilePath, forKey: "iosProfile") } }
    var iosBundleId: String = UserDefaults.standard.string(forKey: "iosBundleId") ?? "" { didSet { UserDefaults.standard.set(iosBundleId, forKey: "iosBundleId") } }
    var iosDeviceId: String = UserDefaults.standard.string(forKey: "iosDevice") ?? "" { didSet { UserDefaults.standard.set(iosDeviceId, forKey: "iosDevice") } }
    var iosExportIPA = UserDefaults.standard.bool(forKey: "iosIPA") { didSet { UserDefaults.standard.set(iosExportIPA, forKey: "iosIPA") } }
    var iosUnsigned = UserDefaults.standard.bool(forKey: "iosUnsigned") { didSet { UserDefaults.standard.set(iosUnsigned, forKey: "iosUnsigned") } }
    var iosInstallAfterBuild = UserDefaults.standard.object(forKey: "iosAutoInstall") as? Bool ?? true { didSet { UserDefaults.standard.set(iosInstallAfterBuild, forKey: "iosAutoInstall") } }
    var iosLaunchAfterInstall = UserDefaults.standard.object(forKey: "iosLaunch") as? Bool ?? true { didSet { UserDefaults.standard.set(iosLaunchAfterInstall, forKey: "iosLaunch") } }
    var installing = false
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
        if target.isIOS { Task { await refreshIOS() } }
        showSetup = sets.isEmpty
        if !args.contains("--no-engine-setup") { setUpEngine() }
        if let d = arg("--debug-drop") { Task { await self.add(urls: [URL(fileURLWithPath: d)]) } }
    }

    var selectedSet: Keyset? { sets.first { $0.name == selectedSetName } ?? sets.first }

    func refreshSets() {
        sets = keysets.list()
        if !sets.contains(where: { $0.name == selectedSetName }) { selectedSetName = sets.first?.name ?? "" }
    }

    // MARK: first-launch engine setup (EngineInstaller)
    /// Copies the bundled CLI/runtime/launcher to Application Support and exports NAPPLE_HOME / NAPPLE_CLI, once per Studio version.
    private func setUpEngine() {
        guard let res = Bundle.main.resourceURL else { return }
        let info = Bundle.main.infoDictionary
        let version = [info?["CFBundleShortVersionString"] as? String, info?["CFBundleVersion"] as? String].compactMap { $0 }.joined(separator: "-")
        let installer = EngineInstaller(resources: res)
        guard installer.bundleHasEngine, !version.isEmpty else { return }
        let firstRun = !installer.isUpToDate(version: version)
        if firstRun { engineMessage = "Setting up the Napple engine..." }
        Task.detached {
            let result = Result { try installer.install(version: version) }
            await MainActor.run {
                switch result {
                case .success(let r):
                    self.engineMessage = r.installed ? "Napple engine \(r.version) is ready. The napple command works in new Terminal windows." : nil
                    r.notes.forEach { self.log.append("engine: \($0)") }
                case .failure(let e): self.engineMessage = "Engine setup failed: \(e.localizedDescription)"
                }
            }
        }
    }

    /// Running straight from the mounted disk image: offer to copy into /Applications and relaunch from there.
    func offerMoveToApplications() {
        let path = Bundle.main.bundleURL.path
        guard path.hasPrefix("/Volumes/"), !CommandLine.arguments.contains("--no-engine-setup") else { return }
        let a = NSAlert()
        a.messageText = "Move Napple Studio to Applications?"
        a.informativeText = "Studio is running from the disk image. It will copy itself to /Applications, set itself up and reopen from there."
        a.addButton(withTitle: "Move to Applications"); a.addButton(withTitle: "Not now")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let dest = URL(fileURLWithPath: "/Applications/NappleStudio.app")
        do {
            if FileManager.default.fileExists(atPath: dest.path) { try FileManager.default.trashItem(at: dest, resultingItemURL: nil) }
            try FileManager.default.copyItem(at: Bundle.main.bundleURL, to: dest)
            _ = try? Process.run(URL(fileURLWithPath: "/usr/bin/xattr"), arguments: ["-dr", "com.apple.quarantine", dest.path])
            NSWorkspace.shared.openApplication(at: dest, configuration: NSWorkspace.OpenConfiguration()) { _, _ in
                DispatchQueue.main.async { NSApp.terminate(nil) }
            }
        } catch { engineMessage = "Could not move to Applications: \(error.localizedDescription). Drag it there manually." }
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

    // MARK: iOS signing and devices
    var selectedProfile: SigningProfile? { signing?.profiles.first { $0.path == iosProfilePath } }
    var selectedDevice: IOSDevice? { devices.first { $0.id == iosDeviceId } }
    /// Bundle id the build will use: the field, else the explicit id of the chosen profile, else Napple's default (fine for wildcard profiles).
    var effectiveBundleId: String? {
        if !iosBundleId.trimmingCharacters(in: .whitespaces).isEmpty { return iosBundleId.trimmingCharacters(in: .whitespaces) }
        return selectedProfile?.explicitBundleId
    }
    /// Why an iOS build cannot start yet (nil = fine).
    var iosBlocker: String? {
        guard target.isIOS else { return nil }
        if iosUnsigned { return nil }
        if cli == nil { return CLIError.notFound.localizedDescription }
        if signing?.identities.isEmpty ?? true { return "No code-signing identity found. In Xcode: Settings > Accounts > add your Apple ID > Manage Certificates > + Apple Development." }
        if iosIdentity.isEmpty { return "Choose a signing identity." }
        guard let prof = selectedProfile else {
            return "Choose a provisioning profile. Create one by running any app on your device from Xcode with that bundle id (a free Apple ID works; the profile then lasts 7 days)."
        }
        if let b = effectiveBundleId, !prof.covers(bundleId: b) { return "The profile is for \(prof.appId); the bundle id is \(b)." }
        return nil
    }

    func refreshIOS() async {
        if let cli { signing = try? await cli.listSigning() }
        if let s = signing {
            if !s.identities.contains(where: { $0.sha1 == iosIdentity }) { iosIdentity = s.identities.first?.sha1 ?? "" }
            if !s.profiles.contains(where: { $0.path == iosProfilePath }) { iosProfilePath = s.profiles.first?.path ?? "" }
        }
        await refreshDevices()
    }
    func refreshDevices() async {
        devices = (try? await NappleCLI.listDevices()) ?? []
        if !devices.contains(where: { $0.id == iosDeviceId }) { iosDeviceId = devices.first(where: { $0.connected })?.id ?? devices.first?.id ?? "" }
    }

    var builtIPA: URL? { builtApp.map { $0.deletingPathExtension().appendingPathExtension("ipa") }.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil } }

    func installToDevice() {
        guard let app = builtApp, let dev = selectedDevice else { installMessage = "Choose a connected device first."; return }
        if !dev.supported { installMessage = "\(dev.name) runs iOS \(dev.osVersion); Napple apps need iOS or iPadOS 26 or later."; return }
        if !dev.developerMode { installMessage = "Turn on Developer Mode on \(dev.name): Settings > Privacy & Security > Developer Mode, then restart it."; return }
        installing = true; installMessage = "Installing on \(dev.name)..."
        let id = NappleCLI.bundleId(of: app), launch = iosLaunchAfterInstall
        Task {
            do { try await NappleCLI.installApp(app, bundleId: id, on: dev.id, launch: launch); installMessage = launch ? "Installed and launched on \(dev.name)." : "Installed on \(dev.name)." }
            catch { installMessage = error.localizedDescription }
            installing = false
        }
    }

    // MARK: build
    func startBuild() {
        guard let plan, let set = selectedSet, let cli else { return }
        guard legalAck else { showLegal = true; return }
        if let msg = diskPreflight(plan) { errorText = msg; phase = .failed; finishDebug(1); return }
        let out = outputDir
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        if let why = iosBlocker { errorText = why; phase = .failed; finishDebug(1); return }
        var ios: IOSOptions?
        if target.isIOS {
            ios = IOSOptions(identity: iosIdentity.isEmpty ? nil : iosIdentity, profile: selectedProfile.map { URL(fileURLWithPath: $0.path) },
                             bundleId: effectiveBundleId, ipa: iosExportIPA || iosUnsigned, unsigned: iosUnsigned)
        }
        let req = BuildRequest(files: expand(files), keyset: set,
                               updates: plan.updates.filter(\.selected).map(\.version),
                               dlc: plan.dlc.filter(\.selected).map(\.titleId), target: target.rawValue, output: out, ios: ios)
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
                if target.isIOS, iosInstallAfterBuild, !iosUnsigned, selectedDevice?.connected == true { installToDevice() }
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
