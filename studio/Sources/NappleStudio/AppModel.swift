import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

enum Phase: Equatable { case idle, planning, ready, building, done, failed }

/// Everything lives in one window; this is the page it shows.
enum Screen: Equatable { case welcome, library, add, plan, building, done, settings }

/// Where a build goes. iOS destinations need iOS or iPadOS 26 or later.
enum Destination: Hashable {
    case mac
    case device(String)     // CoreDevice identifier
    case ipaIPhone
    case ipaIPad

    var key: String {
        switch self {
        case .mac: return "mac"
        case .device(let id): return "device:" + id
        case .ipaIPhone: return "ipa-iphone"
        case .ipaIPad: return "ipa-ipad"
        }
    }
    init?(key: String) {
        switch key {
        case "mac": self = .mac
        case "ipa-iphone": self = .ipaIPhone
        case "ipa-ipad": self = .ipaIPad
        default:
            guard key.hasPrefix("device:"), key.count > 7 else { return nil }
            self = .device(String(key.dropFirst(7)))
        }
    }
    var isIOS: Bool { self != .mac }
}

/// One card in the library.
struct LibraryItem: Identifiable, Equatable {
    var entry: LibraryEntry
    var status: LibraryStatus
    var compat: CompatEntry?
    var id: UUID { entry.id }
    var url: URL? { status == .present ? URL(fileURLWithPath: entry.path) : nil }
}

@MainActor
@Observable
final class AppModel {
    let keysets: KeysetManager
    let cli: NappleCLI?
    let library: LibraryStore

    // MARK: navigation
    var screen: Screen = .library
    private var settingsReturn: Screen = .library
    var toast: String?
    private var toastTask: Task<Void, Never>?
    var dropTargeted = false
    /// Files dropped before any keys existed; planned as soon as the first set is saved.
    private var pendingDrops: [URL] = []

    // MARK: keys
    var sets: [Keyset] = []
    var selectedSetName: String = ""

    // MARK: plan and build
    var files: [URL] = []
    var plan: PlanResult?
    var phase: Phase = .idle
    var errorText: String?
    var inputNote: String?
    var progress: ProgressEvent?
    var log: [String] = []
    var notes: [String] = []
    var builtApp: URL?
    var builtInfo: BuiltAppInfo?
    var builtBytes: UInt64?
    var builtSummary = ""
    var buildStarted: Date?
    var buildDuration: TimeInterval?
    var cancelling = false
    var compat: CompatEntry?
    var showLegal = false
    var installMessage: String?
    var installing = false
    var engineMessage: String?
    private(set) var matrix: CompatMatrix?
    private var buildTask: Task<Void, Never>?
    private var activity: NSObjectProtocol?
    private var returnHomeOnCancel = false

    // MARK: destination, iOS
    var destination: Destination = .mac {
        didSet {
            UserDefaults.standard.set(destination.key, forKey: "destination")
            if case .device(let id) = destination { iosDeviceId = id }
            if destination.isIOS, signing == nil { Task { await refreshIOS() } }
        }
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

    // MARK: library
    var libraryItems: [LibraryItem] = []
    private var watcher: DirectoryWatcher?
    private var libraryLoaded = false

    // MARK: settings
    var legalAck: Bool { didSet { UserDefaults.standard.set(legalAck, forKey: "legalAck") } }
    var outputDir: URL {
        didSet { UserDefaults.standard.set(outputDir.path, forKey: "outputDir"); refreshLibrary(adoptOutput: true) }
    }
    var autoBuild = false
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

    // MARK: setup

    init() {
        let args = CommandLine.arguments
        func arg(_ n: String) -> String? { args.firstIndex(of: n).flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } }
        let support = arg("--support-dir").map { URL(fileURLWithPath: $0) } ?? KeysetManager.defaultRoot
        keysets = KeysetManager(root: support)
        library = LibraryStore(file: support.appendingPathComponent("library.json"))
        cli = NappleCLI.locate()
        outputDir = arg("--output-dir").map { URL(fileURLWithPath: $0) }
            ?? UserDefaults.standard.string(forKey: "outputDir").map { URL(fileURLWithPath: $0) } ?? Self.defaultOutput
        autoBuild = args.contains("--debug-autobuild")
        legalAck = args.contains("--debug-ack") || UserDefaults.standard.bool(forKey: "legalAck")
        if let k = arg("--debug-keys") {
            _ = try? keysets.save(name: "Debug", keys: URL(fileURLWithPath: k), titleKeys: nil,
                                  firmware: arg("--debug-firmware").map { URL(fileURLWithPath: $0) }, replace: true)
        }
        if let saved = UserDefaults.standard.string(forKey: "destination"), let d = Destination(key: saved) { destination = d }
        matrix = Self.matrixURL.flatMap { CompatMatrix.load(from: $0) }
        refreshSets()
        refreshLibrary(adoptOutput: true)
        libraryLoaded = true
        screen = (sets.isEmpty && libraryItems.isEmpty) ? .welcome : .library
        if destination.isIOS { Task { await refreshIOS() } }
        if !args.contains("--no-engine-setup") { setUpEngine() }
        watcher = DirectoryWatcher { [weak self] in Task { @MainActor in self?.refreshLibrary() } }
        syncWatcher()
        if let d = arg("--debug-drop") { Task { await self.handleDrop([URL(fileURLWithPath: d)]) } }
    }

    var selectedSet: Keyset? { sets.first { $0.name == selectedSetName } ?? sets.first }

    func refreshSets() {
        sets = keysets.list()
        if !sets.contains(where: { $0.name == selectedSetName }) { selectedSetName = sets.first?.name ?? "" }
    }

    /// Called when a key set was saved or removed.
    func keysChanged() {
        refreshSets()
        if screen == .welcome, !sets.isEmpty {
            screen = .library
            if !pendingDrops.isEmpty { let p = pendingDrops; pendingDrops = []; Task { await handleDrop(p) } }
        }
        if sets.isEmpty, screen == .plan { screen = .library; resetPlanState() }
    }

    // MARK: engine (EngineInstaller)

    /// "Napple engine 0.1.0 is ready" style status for Settings; nil while unknown.
    var engineStatus: String {
        guard let cli else { return "Not found. Rebuild Studio with build_app.sh or set NAPPLE_CLI." }
        return "Using \(cli.executable.path)"
    }

    /// Copies the bundled CLI/runtime/launcher to Application Support and exports NAPPLE_HOME / NAPPLE_CLI, once per Studio version.
    private func setUpEngine() {
        guard let res = Bundle.main.resourceURL else { return }
        let info = Bundle.main.infoDictionary
        let version = [info?["CFBundleShortVersionString"] as? String, info?["CFBundleVersion"] as? String].compactMap { $0 }.joined(separator: "-")
        let installer = EngineInstaller(resources: res)
        guard installer.bundleHasEngine, !version.isEmpty else { return }
        let firstRun = !installer.isUpToDate(version: version)
        if firstRun { engineMessage = "Setting up the Napple engine…" }
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

    // MARK: toast

    func say(_ text: String) {
        toast = text
        toastTask?.cancel()
        toastTask = Task {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            if !Task.isCancelled { toast = nil }
        }
    }

    // MARK: navigation

    var canGoBack: Bool {
        switch screen {
        case .welcome: return !libraryItems.isEmpty || !sets.isEmpty
        case .library: return false
        case .add, .plan, .building, .done, .settings: return true
        }
    }

    func goBack() {
        switch screen {
        case .settings: screen = settingsReturn == .settings ? .library : settingsReturn
        case .plan, .add, .done, .welcome:
            resetPlanState()
            pendingDrops = []
            screen = .library
        case .building:
            if phase == .building {
                let a = NSAlert()
                a.messageText = "Cancel this build and go home?"
                a.informativeText = "The build stops and nothing is saved."
                a.addButton(withTitle: "Keep Building"); a.addButton(withTitle: "Cancel Build")
                guard a.runModal() == .alertSecondButtonReturn else { return }
                returnHomeOnCancel = true
                cancelBuild()
            } else {
                resetPlanState()
                screen = .library
            }
        case .library: break
        }
    }

    func openSettings() {
        if screen == .settings { goBack(); return }
        settingsReturn = screen == .welcome ? .library : screen
        screen = .settings
    }

    /// "Add game" in the library: the add screen, or key setup first when there are no keys yet.
    func showAdd() {
        guard phase != .building else { say("A build is running. Wait for it to finish or cancel it first."); return }
        resetPlanState()
        screen = sets.isEmpty ? .welcome : .add
    }

    func pickFilesAndAdd() {
        let urls = pick(files: true, dirs: true, multiple: true, message: "Choose game files or folders (.nsp .xci .nsz .xcz)")
        if !urls.isEmpty { Task { await handleDrop(urls) } }
    }

    // MARK: adding games

    /// Files or folders dropped on the window, chosen in a picker, or passed on the command line.
    func handleDrop(_ urls: [URL]) async {
        dropTargeted = false
        guard phase != .building else { say("A build is running. Wait for it to finish or cancel it first."); return }
        let found = InputExpander.expand(urls)
        if found.files.isEmpty {
            errorText = found.unreadable.isEmpty
                ? "No game files in there. Napple reads .nsp, .xci, .nsz and .xcz files, or folders that hold them."
                : "Those files could not be read. They may have been moved or deleted."
            if screen != .plan { screen = sets.isEmpty ? .welcome : .add }
            return
        }
        guard !sets.isEmpty else {
            pendingDrops = urls
            say("Add your keys and firmware first. Your files are ready and will be read right after.")
            screen = .welcome
            return
        }
        // dropping while a plan is on screen adds updates or DLC to it; anywhere else starts a new game
        if screen != .plan || phase == .done { resetPlanState() }
        var all = files
        for u in found.files where !all.contains(where: { $0.standardizedFileURL.path == u.standardizedFileURL.path }) { all.append(u) }
        files = all
        inputNote = found.skippedNote
        screen = .plan
        await replan()
    }

    func resetPlanState() {
        guard phase != .building else { return }
        files = []; plan = nil; phase = .idle; errorText = nil; inputNote = nil; progress = nil; log = []; notes = []
        builtApp = nil; builtInfo = nil; builtBytes = nil; compat = nil; installMessage = nil; installing = false; cancelling = false
        builtSummary = ""; buildDuration = nil
    }

    /// "Build another game" and "Start over".
    func startOver() { resetPlanState(); screen = sets.isEmpty ? .welcome : .add }

    func replan() async {
        errorText = nil
        guard let set = selectedSet else { errorText = "Add keys first in Settings."; pendingDrops = []; screen = .welcome; return }
        guard let cli else { errorText = CLIError.notFound.localizedDescription; phase = .failed; return }
        phase = .planning
        do {
            let p = try await cli.plan(files: files, keys: set.keysURL)
            plan = p
            phase = .ready
            compat = p.base.flatMap { matrix?.entry(for: $0.titleId) }
            if p.base == nil { errorText = nil }
            if autoBuild { autoBuild = false; startBuild() }
        } catch {
            plan = nil
            phase = .failed
            errorText = error.localizedDescription
            if CommandLine.arguments.contains("--debug-autobuild") { finishDebug(1) }
        }
    }

    func toggle(updates: Bool, id: String) {
        guard var p = plan, phase != .building else { return }
        if updates, let i = p.updates.firstIndex(where: { $0.id == id }) { p.updates[i].selected.toggle() }
        if !updates, let i = p.dlc.firstIndex(where: { $0.id == id }) { p.dlc[i].selected.toggle() }
        plan = p
    }

    /// Name shown for the game being planned: the matrix name, else its title id.
    var planTitle: String {
        if let n = compat?.name, !n.isEmpty { return n }
        if let id = plan?.base?.titleId { return "Title \(id)" }
        return "Your game"
    }

    // MARK: iOS signing and devices

    var selectedProfile: SigningProfile? { signing?.profiles.first { $0.path == iosProfilePath } }
    var selectedDevice: IOSDevice? { devices.first { $0.id == iosDeviceId } }
    var target: BuildTarget {
        switch destination {
        case .mac: return .macos
        case .device: return .ios
        case .ipaIPhone: return .iphone
        case .ipaIPad: return .ipad
        }
    }
    var wantsIPA: Bool { destination == .ipaIPhone || destination == .ipaIPad || iosExportIPA || iosUnsigned }
    var destinationLabel: String {
        switch destination {
        case .mac: return "This Mac"
        case .device(let id): return devices.first { $0.id == id }?.name ?? "Selected device"
        case .ipaIPhone: return "iOS .ipa file"
        case .ipaIPad: return "iPadOS .ipa file"
        }
    }

    /// Bundle id the build will use: the field, else the explicit id of the chosen profile, else Napple's default (fine for wildcard profiles).
    var effectiveBundleId: String? {
        let typed = iosBundleId.trimmingCharacters(in: .whitespaces)
        return typed.isEmpty ? selectedProfile?.explicitBundleId : typed
    }

    /// Why this destination cannot build yet (nil = fine).
    var destinationBlocker: String? {
        guard destination.isIOS else { return nil }
        if cli == nil { return CLIError.notFound.localizedDescription }
        if case .device = destination {
            guard let dev = selectedDevice else { return "That device is not connected any more. Choose another destination or reconnect it." }
            if !dev.connected { return "\(dev.name) is not connected. Plug it in and unlock it." }
            if !dev.supported { return "\(dev.name) runs iOS \(dev.osVersion); Napple apps need iOS or iPadOS 26 or later." }
            if !dev.developerMode { return "Turn on Developer Mode on \(dev.name): Settings > Privacy & Security > Developer Mode, then restart it." }
            if iosUnsigned { return "Unsigned apps cannot be installed directly. Turn off Unsigned in Signing, or save an .ipa instead." }
        }
        if iosUnsigned { return nil }
        if signing?.identities.isEmpty ?? true { return "No code-signing identity found. In Xcode: Settings > Accounts > add your Apple ID > Manage Certificates > + Apple Development." }
        if iosIdentity.isEmpty { return "Choose a signing identity in Signing." }
        guard let prof = selectedProfile else {
            return "Choose a provisioning profile in Signing. Create one by running any app on your device from Xcode with that bundle id (a free Apple ID works; the profile then lasts 7 days)."
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
        if iosDeviceId.isEmpty || !devices.contains(where: { $0.id == iosDeviceId }) {
            iosDeviceId = devices.first(where: { $0.connected && $0.supported })?.id ?? devices.first?.id ?? iosDeviceId
        }
    }

    var builtIPA: URL? {
        builtApp.map { $0.deletingPathExtension().appendingPathExtension("ipa") }.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
    }

    func installToDevice() {
        guard let app = builtApp, let dev = selectedDevice else { installMessage = "Choose a connected device first."; return }
        if !dev.supported { installMessage = "\(dev.name) runs iOS \(dev.osVersion); Napple apps need iOS or iPadOS 26 or later."; return }
        if !dev.developerMode { installMessage = "Turn on Developer Mode on \(dev.name): Settings > Privacy & Security > Developer Mode, then restart it."; return }
        installing = true; installMessage = "Installing on \(dev.name)…"
        let id = NappleCLI.bundleId(of: app), launch = iosLaunchAfterInstall
        Task {
            do { try await NappleCLI.installApp(app, bundleId: id, on: dev.id, launch: launch); installMessage = launch ? "Installed and launched on \(dev.name)." : "Installed on \(dev.name)." }
            catch { installMessage = error.localizedDescription }
            installing = false
        }
    }

    // MARK: build

    var diskWarning: String? { plan.flatMap { diskPreflight($0) } }

    var outputWarning: String? {
        let p = outputDir.path, home = FileManager.default.homeDirectoryForCurrentUser.path
        if p.hasPrefix(home + "/Desktop") || p.hasPrefix(home + "/Documents") || p.contains("Mobile Documents") {
            return "The output folder is in a synced location. Pick a folder outside Desktop and Documents so the app's signature stays valid."
        }
        return nil
    }

    var canBuild: Bool {
        plan?.base != nil && phase != .planning && phase != .building && selectedSet != nil && cli != nil
    }

    func startBuild() {
        guard phase != .building else { return }
        guard let plan, let set = selectedSet, let cli else { return }
        guard plan.base != nil else { errorText = "Add the base game before building."; return }
        guard legalAck else { showLegal = true; return }
        if let msg = diskPreflight(plan) { errorText = msg; phase = .failed; finishDebug(1); return }
        if let why = destinationBlocker { errorText = why; phase = .failed; finishDebug(1); return }
        let out = outputDir
        do { try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true) }
        catch { errorText = "Cannot create the output folder \(out.path): \(error.localizedDescription) Choose another folder in Settings."; phase = .failed; finishDebug(1); return }
        guard FileManager.default.isWritableFile(atPath: out.path) else {
            errorText = "Studio cannot write to \(out.path). Choose another output folder in Settings."; phase = .failed; finishDebug(1); return
        }
        var ios: IOSOptions?
        if target.isIOS {
            ios = IOSOptions(identity: iosIdentity.isEmpty ? nil : iosIdentity, profile: selectedProfile.map { URL(fileURLWithPath: $0.path) },
                             bundleId: effectiveBundleId, ipa: wantsIPA, unsigned: iosUnsigned)
        }
        let req = BuildRequest(files: files, keyset: set, updates: plan.selectedUpdateVersions, dlc: plan.selectedDLCIds,
                               target: target.rawValue, output: out, ios: ios)
        builtSummary = plan.includedSummary
        phase = .building; screen = .building
        errorText = nil; notes = []; installMessage = nil; cancelling = false
        builtApp = nil; builtInfo = nil; builtBytes = nil; buildDuration = nil
        buildStarted = Date()
        progress = ProgressEvent(stage: "preflight", percent: 0, message: "Checking keys and firmware"); log = []
        openLog()
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Building a Napple app")
        let goesToMac = target == .macos
        buildTask = Task {
            defer { if let a = activity { ProcessInfo.processInfo.endActivity(a); activity = nil } }
            do {
                try await cli.check(keyset: set)
                let app = try await cli.build(req) { ev in
                    Task { @MainActor in self.handle(ev) }
                }
                try? await Task.sleep(nanoseconds: 200_000_000)
                let result = app ?? firstApp(in: out)
                guard let result, FileManager.default.fileExists(atPath: result.path) else {
                    throw CLIError.failed(code: 1, message: "The build finished but no app was written to \(out.path). Check the log for warnings.")
                }
                builtApp = result
                buildDuration = buildStarted.map { Date().timeIntervalSince($0) }
                builtInfo = BuiltAppInfo.read(result)
                phase = .done
                screen = .done
                writeLog("DONE \(result.path)")
                if goesToMac { await registerInLibrary(result) }
                else { builtBytes = await Task.detached { BuiltAppInfo.sizeOnDisk(result) }.value }
                if case .device = destination, iosInstallAfterBuild, !iosUnsigned, selectedDevice?.connected == true { installToDevice() }
                if !NSApp.isActive { NSApp.requestUserAttention(.informationalRequest) }
                finishDebug(0)
            } catch {
                if Task.isCancelled || cancelling || error.localizedDescription == "Build cancelled." {
                    phase = .ready; errorText = nil
                    if returnHomeOnCancel { resetPlanState(); screen = .library } else { screen = .plan }
                    returnHomeOnCancel = false
                    say("Build cancelled.")
                } else {
                    phase = .failed; errorText = error.localizedDescription
                    if !NSApp.isActive { NSApp.requestUserAttention(.informationalRequest) }
                }
                cancelling = false
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

    func cancelBuild() {
        guard phase == .building else { return }
        cancelling = true
        buildTask?.cancel()
    }

    /// Back from a failed build to the plan (keeps the choices).
    func backToPlan() { errorText = nil; phase = plan == nil ? .idle : .ready; screen = plan == nil ? .library : .plan }

    private func handle(_ ev: BuildEvent) {
        switch ev {
        case .progress(let p):
            progress = p
            let l = "[\(p.stage)] \(Int(p.percent))% \(p.message)"
            appendLog(l); writeLog(l)
        case .result(let p): appendLog("result: \(p)"); writeLog("RESULT \(p)")
        case .log(let l):
            appendLog(l); writeLog(l)
            if l.hasPrefix("note:") || l.hasPrefix("warning:") { notes.append(l) }
        }
    }

    private func appendLog(_ l: String) {
        log.append(l)
        if log.count > 2000 { log.removeFirst(500) }
    }

    private func firstApp(in dir: URL) -> URL? {
        let apps = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []).filter { $0.pathExtension == "app" }
        return apps.max { a, b in
            let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return da < db
        }
    }

    private func finishDebug(_ code: Int32) {
        guard CommandLine.arguments.contains("--debug-autobuild") else { return }
        let delay = Double(CommandLine.arguments.firstIndex(of: "--debug-exit-delay").flatMap { CommandLine.arguments[safe: $0 + 1] } ?? "0") ?? 0
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { exit(code) }
    }

    // MARK: logs

    private func openLog() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        let u = Self.logsDir.appendingPathComponent("build-\(f.string(from: Date())).log")
        FileManager.default.createFile(atPath: u.path, contents: nil, attributes: [.posixPermissions: 0o600])
        try? logFile?.close()
        logFile = try? FileHandle(forWritingTo: u)
        latestLog = u
    }
    private(set) var latestLog: URL?
    private static let logTime: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f }()
    private func writeLog(_ l: String) { logFile?.write(Data((Self.logTime.string(from: Date()) + " " + l + "\n").utf8)) }

    func showLogsInFinder() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        if let l = latestLog, FileManager.default.fileExists(atPath: l.path) { NSWorkspace.shared.activateFileViewerSelecting([l]) }
        else { NSWorkspace.shared.open(Self.logsDir) }
    }

    // MARK: library

    /// Re-reads the library file state: follows moved apps, flags missing ones, optionally adopts apps found in the output folder.
    func refreshLibrary(adoptOutput: Bool = false) {
        if adoptOutput { library.adopt(directory: outputDir) }
        let changes = library.refresh()
        reloadItems()
        syncWatcher()
        guard libraryLoaded else { return }
        for c in changes {
            switch c.kind {
            case .trashed: say("\(c.name) was moved to the Trash, so it left your library.")
            case .moved: say("Found \(c.name) at its new place.")
            case .missing: say("\(c.name) can't be found any more. Locate it or remove it from the library.")
            case .restored: break
            }
        }
    }

    private func reloadItems() {
        libraryItems = library.entries.map { e in
            LibraryItem(entry: e, status: library.status(of: e.id), compat: matrix?.entry(for: e.titleId))
        }
        if libraryItems.isEmpty, screen == .library, sets.isEmpty { screen = .welcome }
    }

    private func syncWatcher() {
        var dirs = library.watchedDirectories
        if FileManager.default.fileExists(atPath: outputDir.path) { dirs.insert(outputDir.path) }
        watcher?.watch(dirs)
    }

    private func registerInLibrary(_ app: URL) async {
        guard let info = BuiltAppInfo.read(app), info.isMac else { return }
        let bytes = await Task.detached { BuiltAppInfo.sizeOnDisk(app) }.value
        builtBytes = bytes
        do { try library.add(app: app, info: info, bytes: bytes) }
        catch { installMessage = "Built, but it could not be added to the library: \(error.localizedDescription)" }
        reloadItems(); syncWatcher()
    }

    // MARK: launching and managing apps

    func launch(_ url: URL?) {
        guard let url else { return }
        guard FileManager.default.fileExists(atPath: url.path) else { say("That app is gone. Refreshing your library."); refreshLibrary(); return }
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration()) { _, error in
            if let error { DispatchQueue.main.async { self.say("Could not open the app: \(error.localizedDescription)") } }
        }
    }

    func launch() { launch(builtApp) }

    func reveal(_ url: URL?) {
        guard let url, FileManager.default.fileExists(atPath: url.path) else { say("That app is gone. Refreshing your library."); refreshLibrary(); return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
    func reveal() { reveal(builtApp) }

    func installToApplications(_ source: URL? = nil) {
        guard let a = source ?? builtApp else { return }
        let dest = URL(fileURLWithPath: "/Applications").appendingPathComponent(a.lastPathComponent)
        if dest.standardizedFileURL == a.standardizedFileURL { installMessage = "It is already in /Applications."; return }
        do {
            if FileManager.default.fileExists(atPath: dest.path) { try FileManager.default.trashItem(at: dest, resultingItemURL: nil) }
            try FileManager.default.copyItem(at: a, to: dest)
            installMessage = "Installed to /Applications."
        } catch { installMessage = "Install failed: \(error.localizedDescription) You can drag the app into Applications yourself." }
        if source != nil, let m = installMessage { say(m) }
    }

    func removeFromLibrary(_ id: UUID) { library.remove(id); reloadItems(); syncWatcher() }

    func trashFromLibrary(_ item: LibraryItem) {
        guard let url = item.url else { removeFromLibrary(item.id); return }
        do {
            try FileManager.default.trashItem(at: url, resultingItemURL: nil)
            removeFromLibrary(item.id)
            say("Moved \(item.entry.name) to the Trash.")
        } catch { say("Could not move it to the Trash: \(error.localizedDescription)") }
    }

    func locate(_ item: LibraryItem) {
        let picked = pick(files: true, dirs: false, message: "Choose \(item.entry.name).app", types: [.application])
        guard let app = picked.first else { return }
        do { try library.relocate(item.id, to: app); reloadItems(); syncWatcher(); say("Found \(item.entry.name).") }
        catch { say(error.localizedDescription) }
    }
}

extension Array { subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil } }
