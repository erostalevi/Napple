import SwiftUI
import AppKit
import NappleStudioCore

enum Phase: Equatable { case idle, planning, ready, building, done, failed }

/// Where a build goes.
enum Destination: Hashable {
    case mac
    case device(String)
    case ipaIPhone
    case ipaIPad
    var key: String {
        switch self {
        case .mac: "mac"
        case .device(let id): "device:" + id
        case .ipaIPhone: "ipa-iphone"
        case .ipaIPad: "ipa-ipad"
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
    private(set) var cli: NappleCLI?
    let library: LibraryStore
    let tools: ToolRunner
    var devicesService: DeviceService { DeviceService(tools: tools) }

    // navigation: one window with the library; the Add / Build panel slides in on the right
    var panelOpen = false
    var selectedLibraryItem: UUID?
    var settingsTab = "general"   // selected Settings tab
    var toast: String?
    private var toastTask: Task<Void, Never>?
    var dropTargeted = false
    var pendingDrops: [URL] = []
    var showLegal = false
    /// Games started with "Play with Debug": each has a console window with the live log.
    var consoles: [DebugConsole] = []
    /// A console the main window should open a window for (it owns the openWindow action).
    var consoleToOpen: UUID?
    var window: NSWindow? { didSet { guard let w = window, oldValue == nil else { return }; w.setFrameAutosaveName("NappleStudioMain") } }

    // keys
    var sets: [Keyset] = []
    var selectedSetName = ""

    // plan and build
    var files: [URL] = []
    var plan: PlanResult?
    var phase: Phase = .idle
    var errorText: String?
    var inputNote: String?
    var progress: ProgressEvent?
    var stagesSeen: [String] = []
    var log: [String] = []
    @ObservationIgnored var pendingLog: [String] = []
    @ObservationIgnored var logFlushScheduled = false
    @ObservationIgnored var lastProgressStamp = Date.distantPast
    @ObservationIgnored var lastLoggedBucket = -1
    var notes: [String] = []
    var builtApp: URL?
    var builtIPA: URL?
    var builtInfo: BuiltAppInfo?
    var builtBytes: UInt64?
    var builtSummary = ""
    var buildStarted: Date?
    var buildDuration: TimeInterval?
    var cancelling = false
    var compat: CompatEntry?
    var installMessage: String?
    var installing = false
    var installFailed = false
    var engineMessage: String?
    /// Engine builds Studio can build with (its own bundled one + the installed versions) and the one the user picked ("" = the default order of NappleCLI.locate).
    private(set) var engines: [EngineBuild] = []
    var engineChoice: String = UserDefaults.standard.string(forKey: "engineChoice") ?? "" {
        didSet { UserDefaults.standard.set(engineChoice, forKey: "engineChoice"); applyEngineChoice() }
    }
    var selectedEngine: EngineBuild? { engines.first { $0.id == engineChoice } }
    func refreshEngines() {
        guard let res = Bundle.main.resourceURL else { return }
        let info = Bundle.main.infoDictionary
        let version = [info?["CFBundleShortVersionString"] as? String, info?["CFBundleVersion"] as? String].compactMap { $0 }.joined(separator: "-")
        engines = EngineInstaller(resources: res, support: keysets.root).listEngines(bundleVersion: version.isEmpty ? nil : version)
        applyEngineChoice()
    }
    /// The picked engine's CLI does the build (it packs its own runtime and launcher into the game); with no (or a vanished) choice the default lookup applies.
    private func applyEngineChoice() {
        if let e = selectedEngine { cli = NappleCLI(executable: e.cliURL) }
        else { cli = NappleCLI.locate() }
    }
    private(set) var matrix: CompatMatrix?
    var buildTask: Task<Void, Never>?
    var activity: NSObjectProtocol?
    var logFile: FileHandle?
    private(set) var latestLog: URL?

    // destination and iOS
    var destination: Destination = .mac {
        didSet {
            UserDefaults.standard.set(destination.key, forKey: "destination")
            if case .device(let id) = destination { iosDeviceId = id }
            if destination.isIOS, signing == nil { Task { await refreshIOS() } }
        }
    }
    var signing: SigningInfo?
    var devices: [IOSDevice] = []
    var deviceNote: String?
    var xcodeTeams: [XcodeTeam] = []
    var profileDetails: [String: ProfileDetails] = [:]
    var signingMode: SigningMode = SigningMode(rawValue: UserDefaults.standard.string(forKey: "signingMode") ?? "") ?? .automatic { didSet { UserDefaults.standard.set(signingMode.rawValue, forKey: "signingMode") } }
    var teamId: String = UserDefaults.standard.string(forKey: "iosTeam") ?? "" { didSet { UserDefaults.standard.set(teamId, forKey: "iosTeam") } }
    var iosIdentity = UserDefaults.standard.string(forKey: "iosIdentity") ?? "" { didSet { UserDefaults.standard.set(iosIdentity, forKey: "iosIdentity") } }
    var iosProfilePath = UserDefaults.standard.string(forKey: "iosProfile") ?? "" { didSet { UserDefaults.standard.set(iosProfilePath, forKey: "iosProfile") } }
    var iosBundlePrefix = UserDefaults.standard.string(forKey: "iosBundlePrefix") ?? BundleIDs.defaultPrefix { didSet { UserDefaults.standard.set(iosBundlePrefix, forKey: "iosBundlePrefix") } }
    var iosDeviceId = UserDefaults.standard.string(forKey: "iosDevice") ?? "" { didSet { UserDefaults.standard.set(iosDeviceId, forKey: "iosDevice") } }
    var iosDebugBuild = UserDefaults.standard.bool(forKey: "iosDebug") { didSet { UserDefaults.standard.set(iosDebugBuild, forKey: "iosDebug") } }
    /// Choosing a device as the destination means installing on it; this stays true (kept for the build code and old settings).
    var iosInstallAfterBuild = true
    var iosLaunchAfterInstall = UserDefaults.standard.object(forKey: "iosLaunch") as? Bool ?? true { didSet { UserDefaults.standard.set(iosLaunchAfterInstall, forKey: "iosLaunch") } }

    // library
    var libraryItems: [LibraryItem] = []
    var searchText = ""
    var watcher: DirectoryWatcher?
    var libraryLoaded = false

    // settings
    var legalAck: Bool { didSet { UserDefaults.standard.set(legalAck, forKey: "legalAck") } }
    var outputDir: URL {
        didSet { UserDefaults.standard.set(outputDir.path, forKey: "outputDir"); refreshLibrary(adoptOutput: true) }
    }
    var autoBuild = false
    static let defaultOutput = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/Napple", isDirectory: true)
    static let logsDir = KeysetManager.defaultRoot.appendingPathComponent("logs", isDirectory: true)
    /// "Play the game when the build finishes" (Settings > General writes the same key).
    var playAfterBuild: Bool { UserDefaults.standard.bool(forKey: "playAfterBuild") }

    static var matrixURL: URL? {
        var d = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        for _ in 0..<8 {
            let c = d.appendingPathComponent("compat/matrix.json")
            if FileManager.default.fileExists(atPath: c.path) { return c }
            d = d.deletingLastPathComponent()
        }
        return Bundle.main.url(forResource: "matrix", withExtension: "json")
    }

    init(tools: ToolRunner = .system) {
        let args = CommandLine.arguments
        func arg(_ n: String) -> String? { args.firstIndex(of: n).flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } }
        let support = arg("--support-dir").map { URL(fileURLWithPath: $0) } ?? KeysetManager.defaultRoot
        self.tools = tools
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
        restoreActiveSet()
        refreshLibrary(adoptOutput: true)
        libraryLoaded = true
        if destination.isIOS { Task { await refreshIOS() } }
        refreshEngines()
        if !args.contains("--no-engine-setup") { setUpEngine() }
        watcher = DirectoryWatcher { [weak self] in Task { @MainActor in self?.refreshLibrary() } }
        syncWatcher()
        if let d = arg("--debug-drop") { Task { await self.handleDrop([URL(fileURLWithPath: d)]) } }
        if let s = arg("--debug-screen") { Task { await self.applyDebugScreen(s) } }
    }

    // MARK: keys

    /// True until the first key set exists: the first-launch dialog blocks the window until keys are in.
    var needsSetup: Bool { sets.isEmpty }
    /// Keys were set up once and deleted since: the library stays usable, building is paused.
    var keysRemoved: Bool { sets.isEmpty && UserDefaults.standard.bool(forKey: "keysEverSet") }
    var showOnboarding: Bool { sets.isEmpty && !UserDefaults.standard.bool(forKey: "keysEverSet") }
    var selectedSet: Keyset? { sets.first { $0.name == selectedSetName } ?? sets.first }
    var isBuilding: Bool { phase == .building }

    func refreshSets() {
        sets = keysets.list()
        if !sets.isEmpty { UserDefaults.standard.set(true, forKey: "keysEverSet") }
        if !sets.contains(where: { $0.name == selectedSetName }) { selectedSetName = sets.first?.name ?? "" }
    }

    func keysChanged() {
        refreshSets()
        if !sets.isEmpty, !pendingDrops.isEmpty { let p = pendingDrops; pendingDrops = []; Task { await handleDrop(p) } }
    }

    // MARK: engine

    var engineStatus: String {
        guard let cli else { return String(localized: "Not found. Rebuild Studio with build_app.sh or set NAPPLE_CLI.") }
        return cli.executable.path
    }

    private func setUpEngine() {
        guard let res = Bundle.main.resourceURL else { return }
        let info = Bundle.main.infoDictionary
        let version = [info?["CFBundleShortVersionString"] as? String, info?["CFBundleVersion"] as? String].compactMap { $0 }.joined(separator: "-")
        let installer = EngineInstaller(resources: res)
        guard installer.bundleHasEngine, !version.isEmpty else { return }
        if !installer.isUpToDate(version: version) { engineMessage = String(localized: "Setting up the Napple engine…") }
        Task.detached {
            let result = Result { try installer.install(version: version) }
            await MainActor.run {
                switch result {
                case .success(let r):
                    self.engineMessage = r.installed ? String(localized: "Napple engine \(r.version) is ready.") : nil
                    self.refreshEngines()
                    r.notes.forEach { self.log.append("engine: \($0)") }
                case .failure(let e): self.engineMessage = String(localized: "Engine setup failed: \(e.localizedDescription)")
                }
            }
        }
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

    // MARK: adding games

    func pickFilesAndAdd() {
        guard !sets.isEmpty else { say(String(localized: "Add your keys and firmware in Settings first.")); return }
        let urls = pick(files: true, dirs: true, multiple: true, message: String(localized: "Choose game files or folders (.nsp .xci .nsz .xcz)"))
        if !urls.isEmpty { Task { await handleDrop(urls) } }
    }

    func handleDrop(_ urls: [URL]) async {
        dropTargeted = false
        guard !showOnboarding else { return }
        guard phase != .building else { say(String(localized: "A build is running. Wait for it to finish or cancel it first.")); return }
        guard !sets.isEmpty else { say(String(localized: "Your keys were removed. Add them in Settings to build games.")); return }
        let found = InputExpander.expand(urls)
        if found.files.isEmpty {
            say(found.unreadable.isEmpty
                ? String(localized: "No game files in there. Napple reads .nsp, .xci, .nsz and .xcz files, or folders that hold them.")
                : String(localized: "Those files could not be read. They may have been moved or deleted."))
            return
        }
        if phase == .done || phase == .failed { resetPlanState() }
        var all = files
        for u in found.files where !all.contains(where: { $0.standardizedFileURL.path == u.standardizedFileURL.path }) { all.append(u) }
        files = all
        inputNote = found.skippedNote
        panelOpen = true
        await replan()
    }

    /// Bumped whenever the plan is thrown away, so a plan that finishes late is ignored.
    @ObservationIgnored private var planGeneration = 0

    func resetPlanState() {
        guard phase != .building else { return }
        planGeneration += 1
        files = []; plan = nil; phase = .idle; errorText = nil; inputNote = nil; progress = nil; clearLog(); notes = []; stagesSeen = []
        builtApp = nil; builtIPA = nil; builtInfo = nil; builtBytes = nil; compat = nil; installMessage = nil; installing = false; installFailed = false; cancelling = false
        builtSummary = ""; buildDuration = nil
    }

    func replan() async {
        errorText = nil
        guard let set = selectedSet else { errorText = String(localized: "Add keys first in Settings."); phase = .failed; return }
        guard let cli else { errorText = CLIError.notFound.localizedDescription; phase = .failed; return }
        phase = .planning
        let generation = planGeneration
        do {
            let p = try await cli.plan(files: files, keys: set.keysURL)
            guard generation == planGeneration else { return }
            plan = p; phase = .ready
            compat = p.base.flatMap { matrix?.entry(for: $0.titleId) }
            if autoBuild { autoBuild = false; startBuild() }
        } catch {
            guard generation == planGeneration else { return }
            plan = nil; phase = .failed; errorText = error.localizedDescription
            finishDebug(1)
        }
    }

    func toggle(updates: Bool, id: String) {
        guard var p = plan, phase != .building else { return }
        if updates, let i = p.updates.firstIndex(where: { $0.id == id }) { p.updates[i].selected.toggle() }
        if !updates, let i = p.dlc.firstIndex(where: { $0.id == id }) { p.dlc[i].selected.toggle() }
        plan = p
    }

    var planTitle: String {
        if let n = compat?.name, !n.isEmpty { return n }
        if let id = plan?.base?.titleId { return String(localized: "Title \(id)") }
        return String(localized: "Your game")
    }

    // MARK: logs

    func openLog() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        let u = Self.logsDir.appendingPathComponent("build-\(f.string(from: Date())).log")
        FileManager.default.createFile(atPath: u.path, contents: nil, attributes: [.posixPermissions: 0o600])
        try? logFile?.close()
        logFile = try? FileHandle(forWritingTo: u)
        latestLog = u
    }
    private static let logTime: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f }()
    /// The log file is written on its own queue: a build prints hundreds of progress lines a second and the main thread must not wait for the disk.
    private static let logQueue = DispatchQueue(label: "studio.logfile")
    func writeLog(_ l: String) {
        guard let f = logFile else { return }
        let line = Data((Self.logTime.string(from: Date()) + " " + l + "\n").utf8)
        Self.logQueue.async { f.write(line) }
    }
    /// Lines reach the visible log in batches (about 8 per second): one SwiftUI update per batch instead of one per line keeps the window smooth during a build.
    func appendLog(_ l: String) {
        pendingLog.append(l)
        guard !logFlushScheduled else { return }
        logFlushScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { [weak self] in self?.flushLog() }
    }
    func flushLog() {
        logFlushScheduled = false
        guard !pendingLog.isEmpty else { return }
        log.append(contentsOf: pendingLog); pendingLog.removeAll(keepingCapacity: true)
        if log.count > 3000 { log.removeFirst(log.count - 2000) }
    }
    func clearLog() { pendingLog.removeAll(); log = [] }

    func showLogsInFinder() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        if let l = latestLog, FileManager.default.fileExists(atPath: l.path) { NSWorkspace.shared.activateFileViewerSelecting([l]) }
        else { NSWorkspace.shared.open(Self.logsDir) }
    }

    func finishDebug(_ code: Int32) {
        guard CommandLine.arguments.contains("--debug-autobuild") else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(code) }
    }
}

extension Array { subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil } }
