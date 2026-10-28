import SwiftUI
import AppKit
import NappleStudioCore

enum Phase: Equatable { case idle, planning, ready, building, done, failed }

/// Sidebar destinations.
enum Pane: String, Hashable, CaseIterable, Identifiable {
    case library, add, devices
    var id: String { rawValue }
    var title: LocalizedStringKey {
        switch self {
        case .library: "Library"
        case .add: "Add Game"
        case .devices: "iPhone & iPad"
        }
    }
    var symbol: String {
        switch self {
        case .library: "square.grid.2x2"
        case .add: "plus.square.on.square"
        case .devices: "iphone.gen3"
        }
    }
}

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
    /// The game whose Run view (live runtime log) is open.
    var runItem: LibraryItem?

    // navigation
    var pane: Pane = .library
    var selectedLibraryItem: UUID?
    var showInspector = false
    var settingsTab = "general"   // selected Settings tab (the debug tour switches it)
    var toast: String?
    private var toastTask: Task<Void, Never>?
    var dropTargeted = false
    var pendingDrops: [URL] = []
    var showLegal = false
    /// `--debug-tour-size WxH` (default: the app's default window size) sizes the window the tour photographs.
    static func tourSize() -> NSSize {
        let a = CommandLine.arguments
        if let i = a.firstIndex(of: "--debug-tour-size"), i + 1 < a.count {
            let p = a[i + 1].split(separator: "x").compactMap { Double($0) }
            if p.count == 2 { return NSSize(width: p[0], height: p[1]) }
        }
        return NSSize(width: 880, height: 580)
    }
    /// `--debug-tour <dir>`: visit every pane, write a PNG of the window for each, then quit (scripted UI check).
    var tour: URL?
    var window: NSWindow? { didSet { guard let w = window, oldValue == nil else { return }; w.setFrameAutosaveName("NappleStudioMain"); if tour != nil { w.setContentSize(Self.tourSize()); startTour() } } }

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
        engines = EngineInstaller(resources: res).listEngines(bundleVersion: version.isEmpty ? nil : version)
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
    var iosInstallAfterBuild = UserDefaults.standard.object(forKey: "iosAutoInstall") as? Bool ?? true { didSet { UserDefaults.standard.set(iosInstallAfterBuild, forKey: "iosAutoInstall") } }
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
        if let p = arg("--pane"), let pane = Pane(rawValue: p) { self.pane = pane }
        tour = arg("--debug-tour").map { URL(fileURLWithPath: $0) }
        matrix = Self.matrixURL.flatMap { CompatMatrix.load(from: $0) }
        refreshSets()
        refreshLibrary(adoptOutput: true)
        libraryLoaded = true
        if destination.isIOS || self.pane == .devices { Task { await refreshIOS() } }
        refreshEngines()
        if !args.contains("--no-engine-setup") { setUpEngine() }
        watcher = DirectoryWatcher { [weak self] in Task { @MainActor in self?.refreshLibrary() } }
        syncWatcher()
        if let d = arg("--debug-drop") { Task { await self.handleDrop([URL(fileURLWithPath: d)]) } }
    }

    var needsSetup: Bool { sets.isEmpty }
    var selectedSet: Keyset? { sets.first { $0.name == selectedSetName } ?? sets.first }
    var isBuilding: Bool { phase == .building }

    func refreshSets() {
        sets = keysets.list()
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
        let urls = pick(files: true, dirs: true, multiple: true, message: String(localized: "Choose game files or folders (.nsp .xci .nsz .xcz)"))
        if !urls.isEmpty { Task { await handleDrop(urls) } }
    }

    func handleDrop(_ urls: [URL]) async {
        dropTargeted = false
        guard phase != .building else { say(String(localized: "A build is running. Wait for it to finish or cancel it first.")); return }
        let found = InputExpander.expand(urls)
        pane = .add
        if found.files.isEmpty {
            errorText = found.unreadable.isEmpty
                ? String(localized: "No game files in there. Napple reads .nsp, .xci, .nsz and .xcz files, or folders that hold them.")
                : String(localized: "Those files could not be read. They may have been moved or deleted.")
            return
        }
        guard !sets.isEmpty else {
            pendingDrops = urls
            say(String(localized: "Add your keys and firmware first. Your files will be read right after."))
            return
        }
        if phase == .done || phase == .failed { resetPlanState() }
        var all = files
        for u in found.files where !all.contains(where: { $0.standardizedFileURL.path == u.standardizedFileURL.path }) { all.append(u) }
        files = all
        inputNote = found.skippedNote
        await replan()
    }

    func resetPlanState() {
        guard phase != .building else { return }
        files = []; plan = nil; phase = .idle; errorText = nil; inputNote = nil; progress = nil; log = []; notes = []; stagesSeen = []
        builtApp = nil; builtIPA = nil; builtInfo = nil; builtBytes = nil; compat = nil; installMessage = nil; installing = false; cancelling = false
        builtSummary = ""; buildDuration = nil
    }

    func replan() async {
        errorText = nil
        guard let set = selectedSet else { errorText = String(localized: "Add keys first in Settings."); return }
        guard let cli else { errorText = CLIError.notFound.localizedDescription; phase = .failed; return }
        phase = .planning
        do {
            let p = try await cli.plan(files: files, keys: set.keysURL)
            plan = p; phase = .ready
            compat = p.base.flatMap { matrix?.entry(for: $0.titleId) }
            if autoBuild { autoBuild = false; startBuild() }
        } catch {
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
    func writeLog(_ l: String) { logFile?.write(Data((Self.logTime.string(from: Date()) + " " + l + "\n").utf8)) }
    func appendLog(_ l: String) { log.append(l); if log.count > 3000 { log.removeFirst(1000) } }

    func showLogsInFinder() {
        try? FileManager.default.createDirectory(at: Self.logsDir, withIntermediateDirectories: true)
        if let l = latestLog, FileManager.default.fileExists(atPath: l.path) { NSWorkspace.shared.activateFileViewerSelecting([l]) }
        else { NSWorkspace.shared.open(Self.logsDir) }
    }

    // MARK: debug tour

    /// Writes the window's screen rectangle (top-left origin, points) for an external `screencapture -R`, then waits for `<name>.png` to appear.
    func snapshot(_ name: String, of target: NSWindow? = nil) async {
        guard let w = target ?? window, let dir = tour, let screen = w.screen ?? NSScreen.main else { return }
        let f = w.frame
        let top = screen.frame.maxY - f.maxY
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? "\(Int(f.minX)),\(Int(top)),\(Int(f.width)),\(Int(f.height))".write(to: dir.appendingPathComponent("\(name).rect"), atomically: true, encoding: .utf8)
        for _ in 0..<100 {
            if FileManager.default.fileExists(atPath: dir.appendingPathComponent("\(name).png").path) { break }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
    }

    private func startTour() {
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate()
        Task { @MainActor in
            let steps: [(String, () -> Void)] = [
                ("01-library", { self.pane = .library }),
                ("02-add", { self.pane = .add }),
                ("03-devices", { self.pane = .devices; Task { await self.refreshIOS() } }),
                ("04-log", { self.pane = .add; self.showInspector = true }),
            ]
            for (name, go) in steps {
                go()
                try? await Task.sleep(nanoseconds: name.hasSuffix("devices") ? 5_000_000_000 : 1_500_000_000)
                await snapshot(name)
            }
            showInspector = false
            // the Settings window: the Mii, Amiibo and Online tabs over system.json
            let sw = NSWindow(contentViewController: NSHostingController(rootView: SettingsView().environment(self)))
            sw.title = "Settings"
            sw.setFrameOrigin(NSPoint(x: (window?.frame.minX ?? 100) + 40, y: (window?.frame.minY ?? 100) + 40))
            sw.makeKeyAndOrderFront(nil)
            for tab in ["mii", "amiibo", "online"] {
                settingsTab = tab
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                await snapshot("08-settings-\(tab)", of: sw)
            }
            sw.close()
            for _ in 0..<60 where phase == .planning { try? await Task.sleep(nanoseconds: 500_000_000) }
            if CommandLine.arguments.contains("--debug-fake-done") {   // replays the Simulator flow's state changes without building (crash reproduction)
                let skip = Set((ProcessInfo.processInfo.environment["NAPPLE_FAKE_SKIP"] ?? "").split(separator: ",").map(String.init))
                pane = .add
                plan = PlanResult(base: PlanItem(titleId: "01006BB00C6F0000", version: 0, versionLabel: "v0", size: 6_000_000_000, selected: true, path: "/x"), updates: [], dlc: [], warnings: [])
                if !skip.contains("compat") { compat = matrix?.entry(for: "01006BB00C6F0000") }
                destination = .device(devices.first(where: { $0.simulator })?.id ?? devices.first?.id ?? "x")
                builtSummary = plan?.includedSummary ?? ""
                if !skip.contains("building") { phase = .building }
                buildStarted = Date(); stagesSeen = []; log = []
                progress = ProgressEvent(stage: "preflight", percent: 0, message: "Checking keys and firmware")
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                for st in ["ingest", "plan", "rewrite", "romfs", "fonts", "bundle", "sign", "done"] {
                    if !skip.contains("stages") { handle(.progress(ProgressEvent(stage: st, percent: 90, message: "fake \(st)"))) }
                    if !skip.contains("log") { for i in 0..<40 { handle(.log("line \(st) \(i)")) } }
                    try? await Task.sleep(nanoseconds: 150_000_000)
                }
                builtApp = URL(fileURLWithPath: "/tmp/Fake.app"); builtInfo = nil
                if !skip.contains("notes") {
                    notes.append("note: 4 rewritten module(s) packed"); notes.append("note: ios-sim build: ad-hoc signed")
                    notes.append(String(localized: "Simulator build: ad-hoc signed, no memory entitlement (the Simulator has no memory limit)."))
                }
                if !skip.contains("install") { installing = true; installMessage = String(localized: "Installing on iPhone 17…") }
                if !skip.contains("log") { for l in DeviceService.commandLines(app: builtApp!, bundleId: effectiveBundleId, on: selectedDevice ?? devices[0]) { appendLog("$ " + l) } }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                await Task.yield()
                if !skip.contains("install") { installMessage = String(localized: "Installed and launched on iPhone 17."); installing = false }
                buildDuration = buildStarted.map { Date().timeIntervalSince($0) }
                builtBytes = 6_500_000_000
                await Task.yield()
                phase = .done
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                await snapshot("09-fake-done")
            }
            if plan != nil { pane = .add; try? await Task.sleep(nanoseconds: 800_000_000); await snapshot("05-plan") }
            if plan != nil, CommandLine.arguments.contains("--debug-tour-build") {
                startBuild()
                try? await Task.sleep(nanoseconds: 6_000_000_000)
                showInspector = true
                await snapshot("06-building")
                showInspector = false
                for _ in 0..<2400 where phase == .building { try? await Task.sleep(nanoseconds: 500_000_000) }
                try? await Task.sleep(nanoseconds: 800_000_000)
                await snapshot(phase == .done ? "07-done" : "07-failed")
            }
            if !libraryItems.isEmpty { pane = .library; selectedLibraryItem = libraryItems.first?.id; try? await Task.sleep(nanoseconds: 800_000_000); await snapshot("06-library-selected") }
            exit(0)
        }
    }

    func finishDebug(_ code: Int32) {
        guard CommandLine.arguments.contains("--debug-autobuild") else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(code) }
    }
}

extension Array { subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil } }
