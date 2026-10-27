import Foundation

public enum CLIError: Error, LocalizedError, Equatable {
    case notFound
    case notAvailable(String)
    case failed(code: Int32, message: String)
    case badOutput(String)

    public var errorDescription: String? {
        switch self {
        case .notFound: return "The napple command-line tool was not found. Set NAPPLE_CLI or rebuild Studio with build_app.sh."
        case .notAvailable(let c): return "\"napple \(c)\" is not available yet in this version of the core."
        case .failed(_, let m): return m
        case .badOutput(let m): return "Unexpected output from napple: \(m)"
        }
    }
}

/// One parsed `PROGRESS <stage> <percent> <message>` line (see docs/architecture/studio-protocol.md).
public struct ProgressEvent: Equatable, Sendable {
    public var stage: String
    public var percent: Double  // 0...100
    public var message: String
    public init(stage: String, percent: Double, message: String) { self.stage = stage; self.percent = percent; self.message = message }
}

public enum ErrorText {
    /// Turns `error: [code] message` into a human sentence.
    public static func humanize(_ raw: String) -> String {
        var t = raw
        if t.hasPrefix("error:") { t = String(t.dropFirst(6)).trimmingCharacters(in: .whitespaces) }
        var code = ""
        if t.hasPrefix("["), let e = t.firstIndex(of: "]") {
            code = String(t[t.index(after: t.startIndex)..<e]); t = String(t[t.index(after: e)...]).trimmingCharacters(in: .whitespaces)
        }
        let hint: String
        switch code {
        case "keys": hint = "Check the selected key set in Settings (prod.keys may be incomplete or too old for this game)."
        case "firmware": hint = "Check the firmware in the selected set (zip or folder with .nca files)."
        case "disk-space": hint = "Free some disk space or pick another output folder in Settings."
        case "cancelled": return "Build cancelled."
        case "bktr": hint = "Try building without the update (uncheck it)."
        case "runtime": hint = "The runtime or launcher binaries are missing. Rebuild Studio with build_app.sh."
        case "sign": hint = "Signing failed. On macOS choose an output folder outside Desktop/Documents; for iPhone/iPad check the signing identity and that the provisioning profile matches the bundle id."
        case "target": hint = "Choose macOS, iPhone, iPad or iPhone & iPad."
        case "macho": hint = "A module could not be packed for iOS. Rebuild the game with this version of Studio."
        case "ipa": hint = "The .ipa could not be written (ditto failed)."
        case "install": hint = "Unlock the device, trust this Mac, turn on Developer Mode (Settings > Privacy & Security) and, for a free Apple ID, trust the developer in Settings > General > VPN & Device Management."
        case "input", "container", "integrity", "unsupported": hint = "The game file may be damaged or unsupported."
        default: hint = ""
        }
        return hint.isEmpty ? t : "\(t) \(hint)"
    }
}

public enum BuildEvent: Equatable, Sendable {
    case progress(ProgressEvent)
    case result(String)   // `RESULT <path to .app>`
    case log(String)
}

public enum ProgressParser {
    public static func parse(_ line: String) -> BuildEvent {
        let parts = line.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: true)
        if parts.count >= 3, parts[0] == "PROGRESS", let p = Double(parts[2]), p.isFinite {
            return .progress(ProgressEvent(stage: String(parts[1]), percent: min(100, max(0, p)),
                                           message: parts.count > 3 ? String(parts[3]) : ""))
        }
        if line.hasPrefix("RESULT ") {
            let p = String(line.dropFirst(7)).trimmingCharacters(in: .whitespaces)
            if !p.isEmpty { return .result(p) }
        }
        return .log(line)
    }
}

public struct BuildRequest: Sendable {
    public var files: [URL]
    public var keyset: Keyset
    public var updates: [UInt32]       // included update versions (raw)
    public var dlc: [String]           // included DLC title ids (hex)
    public var target: String = "macos"
    public var output: URL
    public var name: String?
    public var ios: IOSOptions?        // only used when target is ios / iphone / ipad
    public init(files: [URL], keyset: Keyset, updates: [UInt32], dlc: [String], target: String = "macos", output: URL, name: String? = nil, ios: IOSOptions? = nil) {
        self.files = files; self.keyset = keyset; self.updates = updates; self.dlc = dlc
        self.target = target; self.output = output; self.name = name; self.ios = ios
    }
}

public struct NappleCLI: Sendable {
    public let executable: URL
    public init(executable: URL) { self.executable = executable }

    /// Resolution order: NAPPLE_CLI env, bundled Resources/napple, installed engine (Application Support/Napple/engine/current), repo target/release, /usr/local/bin, /opt/homebrew/bin.
    public static func locate() -> NappleCLI? {
        var c: [String] = []
        if let e = ProcessInfo.processInfo.environment["NAPPLE_CLI"] { c.append(e) }
        if let r = Bundle.main.resourceURL { c.append(r.appendingPathComponent("napple").path) }
        c.append(EngineInstaller(resources: Bundle.main.resourceURL ?? URL(fileURLWithPath: "/")).cli.path)   // copy placed by first-launch setup
        let exe = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        var d = exe.deletingLastPathComponent()
        for _ in 0..<7 { c.append(d.appendingPathComponent("target/release/napple").path); d = d.deletingLastPathComponent() }
        c += ["/usr/local/bin/napple", "/opt/homebrew/bin/napple"]
        return c.first { FileManager.default.isExecutableFile(atPath: $0) }.map { NappleCLI(executable: URL(fileURLWithPath: $0)) }
    }

    // MARK: commands

    public func plan(files: [URL], keys: URL, updates: String = "all", dlc: String = "all") async throws -> PlanResult {
        let args = ["plan"] + files.map(\.path) + ["-k", keys.path, "--updates", updates, "--dlc", dlc, "--json"]
        let out = try await run(args: args, command: "plan")
        guard let data = out.stdout.data(using: .utf8) else { throw CLIError.badOutput("not UTF-8") }
        do { return try PlanResult.decode(data) } catch { throw CLIError.badOutput("plan JSON could not be decoded") }
    }

    /// Runs `napple build`. Emits events as they arrive; returns the produced .app path when announced.
    public func build(_ r: BuildRequest, onEvent: @escaping @Sendable (BuildEvent) -> Void) async throws -> URL? {
        var args = ["build"] + r.files.map(\.path) + ["-k", r.keyset.keysURL.path]
        if let t = r.keyset.titleKeysURL { args += ["--title-keys", t.path] }
        if let f = r.keyset.firmwareURL { args += ["--firmware", f.path] }
        args += ["--updates", r.updates.isEmpty ? "none" : r.updates.map(String.init).joined(separator: ",")]
        args += ["--dlc", r.dlc.isEmpty ? "none" : r.dlc.joined(separator: ",")]
        args += ["--target", r.target, "-o", r.output.path]
        if let n = r.name, !n.isEmpty { args += ["--name", n] }
        // Bundled helper binaries sit next to the CLI; pass them explicitly when present.
        let dir = executable.deletingLastPathComponent()
        let target = BuildTarget(rawValue: r.target) ?? .macos
        let isIOS = target.isIOS
        // iOS apps have no launcher (no exec on iOS) and use the runtime built by tools/build_ios.sh (device) / build_ios.sh --sim (Simulator)
        let helpers = target.isSimulator ? [("--runtime", "napple-runtime-ios-sim")] : isIOS ? [("--runtime", "napple-runtime-ios")] : [("--runtime", "napple-runtime"), ("--launcher", "napple-launcher")]
        for (flag, file) in helpers {
            let u = dir.appendingPathComponent(file)
            if FileManager.default.isExecutableFile(atPath: u.path) { args += [flag, u.path] }
        }
        if isIOS, !target.isSimulator, let o = r.ios {
            if o.unsigned { args.append("--no-sign") } else {
                if let i = o.identity { args += ["--signing-identity", i] }
                if let p = o.profile { args += ["--provisioning-profile", p.path] }
            }
            if let b = o.bundleId, !b.isEmpty { args += ["--bundle-id", b] }
            if o.ipa { args.append("--ipa") }
        }
        let result = LockedBox<String?>(nil)
        _ = try await run(args: args, command: "build") { line in
            let ev = ProgressParser.parse(line)
            if case .result(let p) = ev { result.value = p }
            onEvent(ev)
        }
        return result.value.map { URL(fileURLWithPath: $0) }
    }

    /// Preflight: `napple build --check` (names/generations only). Throws with a readable message on failure.
    public func check(keyset: Keyset) async throws {
        var args = ["build", "--check", "-k", keyset.keysURL.path]
        if let f = keyset.firmwareURL { args += ["--firmware", f.path] }
        _ = try await run(args: args, command: "build --check")
    }

    // MARK: iOS signing and devices

    /// Code-signing identities and iOS provisioning profiles found on this Mac (`napple build --list-signing --json`).
    public func listSigning() async throws -> SigningInfo {
        let out = try await run(args: ["build", "--list-signing", "--json"], command: "build --list-signing")
        guard let data = out.stdout.split(separator: "\n").last.map(String.init)?.data(using: .utf8) else { throw CLIError.badOutput("empty signing list") }
        do { return try SigningInfo.decode(data) } catch { throw CLIError.badOutput("signing list JSON could not be decoded") }
    }

    /// Phones and tablets known to CoreDevice (`xcrun devicectl list devices`).
    public static func listDevices() async throws -> [IOSDevice] {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("napple-devices-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: file) }
        _ = try await runTool(URL(fileURLWithPath: "/usr/bin/xcrun"), ["devicectl", "list", "devices", "--json-output", file.path])
        guard let data = try? Data(contentsOf: file) else { return [] }
        return DeviceList.parse(data)
    }

    /// `xcrun devicectl device install app` (and optionally `process launch`). Throws a readable message.
    public static func installApp(_ app: URL, bundleId: String?, on device: String, launch: Bool) async throws {
        do { _ = try await runTool(URL(fileURLWithPath: "/usr/bin/xcrun"), ["devicectl", "device", "install", "app", "--device", device, app.path]) }
        catch { throw CLIError.failed(code: 1, message: ErrorText.humanize("error: [install] \(error.localizedDescription)")) }
        if launch, let b = bundleId {
            do { _ = try await runTool(URL(fileURLWithPath: "/usr/bin/xcrun"), ["devicectl", "device", "process", "launch", "--device", device, b]) }
            catch { throw CLIError.failed(code: 1, message: "Installed, but launching failed: \(error.localizedDescription) Trust the developer in Settings > General > VPN & Device Management, then open the app.") }
        }
    }

    /// Reads the bundle id out of a built .app (iOS bundles are flat: Info.plist at the top).
    public static func bundleId(of app: URL) -> String? {
        for p in ["Info.plist", "Contents/Info.plist"] {
            if let d = NSDictionary(contentsOf: app.appendingPathComponent(p)), let b = d["CFBundleIdentifier"] as? String { return b }
        }
        return nil
    }

    static func runTool(_ exe: URL, _ args: [String]) async throws -> Output {
        let p = Process()
        p.executableURL = exe; p.arguments = args
        let o = Pipe(), e = Pipe()
        p.standardOutput = o; p.standardError = e; p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { throw CLIError.failed(code: 127, message: "Could not run \(exe.path): \(error.localizedDescription). Install Xcode and its command line tools.") }
        let out = LockedBox<[String]>([]), err = LockedBox<[String]>([])
        await withTaskGroup(of: Void.self) { g in
            g.addTask { await Self.lines(of: o.fileHandleForReading) { out.append($0) } }
            g.addTask { await Self.lines(of: e.fileHandleForReading) { err.append($0) } }
        }
        p.waitUntilExit()
        let so = out.value.joined(separator: "\n"), se = err.value.joined(separator: "\n")
        if p.terminationStatus != 0 {
            let msg = (se.isEmpty ? so : se).split(separator: "\n").suffix(3).joined(separator: " ")
            throw CLIError.failed(code: p.terminationStatus, message: msg.isEmpty ? "\(exe.lastPathComponent) exited with code \(p.terminationStatus)" : msg)
        }
        return Output(stdout: so, stderr: se)
    }

    // MARK: process plumbing

    struct Output { var stdout: String; var stderr: String }

    /// Calls `onLine` for each line as soon as it arrives; returns at end of file. (`FileHandle.bytes.lines` on stdout and stderr
    /// at once delivered nothing until the process exited, which froze Studio's progress bar for the whole build.)
    static func lines(of h: FileHandle, _ onLine: @escaping @Sendable (String) -> Void) async {
        let buf = LockedBox<Data>(Data())
        @Sendable func emit(_ d: Data) {
            var s = String(decoding: d, as: UTF8.self)
            if s.hasSuffix("\r") { s.removeLast() }
            onLine(s)
        }
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            h.readabilityHandler = { h in
                let d = h.availableData
                var b = buf.value
                if d.isEmpty {
                    h.readabilityHandler = nil
                    if !b.isEmpty { emit(b) }
                    c.resume(); return
                }
                b.append(d)
                while let i = b.firstIndex(of: 0x0A) { emit(b[b.startIndex..<i]); b.removeSubrange(b.startIndex...i) }
                buf.value = b
            }
        }
    }

    func run(args: [String], command: String, onStdoutLine: (@Sendable (String) -> Void)? = nil) async throws -> Output {
        let p = Process()
        p.executableURL = executable
        p.arguments = args
        let o = Pipe(), e = Pipe()
        p.standardOutput = o; p.standardError = e
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { throw CLIError.notFound }
        let out = LockedBox<[String]>([]), err = LockedBox<[String]>([])
        await withTaskCancellationHandler {
            await withTaskGroup(of: Void.self) { g in
                g.addTask { await Self.lines(of: o.fileHandleForReading) { l in out.append(l); onStdoutLine?(l) } }
                g.addTask { await Self.lines(of: e.fileHandleForReading) { l in err.append(l); onStdoutLine?(l) } }
            }
            p.waitUntilExit()
        } onCancel: { p.terminate() }
        let so = out.value.joined(separator: "\n"), se = err.value.joined(separator: "\n")
        if p.terminationStatus != 0 {
            let usage = se.hasPrefix("usage:") || se.contains("unknown command") || se.contains("unrecognized")
            if usage && !(se.contains(command) && se.contains("error")) { throw CLIError.notAvailable(command) }
            let raw = se.split(separator: "\n").last(where: { $0.hasPrefix("error") }).map(String.init)
                ?? se.split(separator: "\n").last.map(String.init) ?? "napple exited with code \(p.terminationStatus)"
            throw CLIError.failed(code: p.terminationStatus, message: ErrorText.humanize(raw))
        }
        return Output(stdout: so, stderr: se)
    }
}

final class LockedBox<T>: @unchecked Sendable {
    private var v: T
    private let l = NSLock()
    init(_ v: T) { self.v = v }
    var value: T { get { l.lock(); defer { l.unlock() }; return v } set { l.lock(); v = newValue; l.unlock() } }
}
extension LockedBox where T == [String] {
    func append(_ s: String) { l.lock(); v.append(s); l.unlock() }
}

// MARK: - System data, hardware simulators, online provider and the per-game run log
// One contract with the runtime: <support>/system.json (runtime/src/services/sysconfig_core.h) and <support>/system/{mii,amiibo,fonts}.
// Studio, the in-game overlay, the launcher and the dev scripts all resolve the same support folder, so nothing here is per game.

public enum SystemData {
    /// NAPPLE_SUPPORT_DIR or ~/Library/Application Support/Napple (what the launcher and the runtime use).
    public static func supportDir(env: [String: String] = ProcessInfo.processInfo.environment) -> URL {
        if let e = env["NAPPLE_SUPPORT_DIR"], !e.isEmpty { return URL(fileURLWithPath: e) }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Napple")
    }
}

/// system.json: the settings of the hardware simulators and the online provider, read by the runtime at boot (the environment still wins there).
public struct SystemConfig: Equatable, Sendable {
    public var amiiboDatabase = ""   // NAPPLE_AMIIBO_DB: URL, .json file or folder
    public var amiiboKey = ""        // NAPPLE_AMIIBO_KEY: key_retail.bin
    public var amiiboStore = ""      // NAPPLE_AMIIBO_STORE: folder of the virtual amiibo (empty = <support>/system/amiibo)
    public var onlineProvider = ""   // NAPPLE_ONLINE_PROVIDER: module path (empty = NullProvider, offline)
    public var miiDir = ""           // NAPPLE_MII_DIR (empty = <support>/system/mii)
    public init() {}
    static var keys: [(String, WritableKeyPath<SystemConfig, String>)] {
        [("amiibo_database", \.amiiboDatabase), ("amiibo_key", \.amiiboKey), ("amiibo_store", \.amiiboStore),
         ("online_provider", \.onlineProvider), ("mii_dir", \.miiDir)]
    }
    public static func url(support: URL) -> URL { support.appendingPathComponent("system.json") }
    public static func load(support: URL = SystemData.supportDir()) -> SystemConfig {
        var c = SystemConfig()
        guard let d = try? Data(contentsOf: url(support: support)), let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return c }
        for (k, p) in keys { if let s = o[k] as? String { c[keyPath: p] = s } }
        return c
    }
    public func save(support: URL = SystemData.supportDir()) throws {
        try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        var o: [String: String] = [:]
        for (k, p) in Self.keys where !self[keyPath: p].isEmpty { o[k] = self[keyPath: p] }
        let d = try JSONSerialization.data(withJSONObject: o, options: [.prettyPrinted, .sortedKeys])
        try d.write(to: Self.url(support: support), options: .atomic)
    }
    /// The online provider as the user typed it: a module that exists, or nothing (the runtime also falls back to the NullProvider with one log line).
    public var onlineProviderUsable: Bool { onlineProvider.isEmpty || FileManager.default.isReadableFile(atPath: onlineProvider) }
    public var effectiveMiiFolder: URL { miiDir.isEmpty ? SystemData.supportDir().appendingPathComponent("system/mii") : URL(fileURLWithPath: miiDir) }
}

/// The shared Mii database (MiiDatabase.dat: 16-byte "NFDB" header, then 0x44-byte StoreData entries; see runtime/src/services/mii_core.c).
public struct MiiStore: Sendable {
    public struct Entry: Equatable, Sendable { public var index: Int; public var name: String }
    public let folder: URL
    public init(folder: URL) { self.folder = folder }
    public var database: URL { folder.appendingPathComponent("MiiDatabase.dat") }
    static let entrySize = 0x44, headerSize = 16

    static func crc16(_ bytes: ArraySlice<UInt8>) -> UInt16 {   // CRC-16/XMODEM
        var crc: UInt16 = 0
        for b in bytes { crc ^= UInt16(b) << 8; for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 } }
        return crc
    }
    private func read() -> [UInt8]? {
        guard let d = try? Data(contentsOf: database) else { return nil }
        let b = [UInt8](d)
        guard b.count >= Self.headerSize, b[0..<4].elementsEqual("NFDB".utf8), b[4] == 1, b.count == Self.headerSize + Int(b[5]) * Self.entrySize else { return nil }
        return b
    }
    public var exists: Bool { read() != nil }
    public func entries() -> [Entry] {
        guard let b = read() else { return [] }
        return (0..<Int(b[5])).map { i in
            let o = Self.headerSize + i * Self.entrySize + 0x1c
            var u: [UInt16] = []
            for k in 0..<10 { let c = UInt16(b[o + 2 * k]) | UInt16(b[o + 2 * k + 1]) << 8; if c == 0 { break }; u.append(c) }
            return Entry(index: i, name: String(decoding: u, as: UTF16.self))
        }
    }
    /// Removes one Mii (keeps the previous file as .bak, like the runtime). The last Mii cannot be deleted: the runtime reseeds an empty database.
    public func delete(index: Int) throws {
        guard var b = read(), index >= 0, index < Int(b[5]) else { throw CLIError.failed(code: 1, message: "That Mii is not in the database.") }
        guard b[5] > 1 else { throw CLIError.failed(code: 1, message: "The last Mii stays: a console always has one.") }
        let off = Self.headerSize + index * Self.entrySize
        b.removeSubrange(off..<(off + Self.entrySize))
        b[5] -= 1
        let crc = Self.crc16(b[Self.headerSize...])
        b[6] = UInt8(crc & 0xff); b[7] = UInt8(crc >> 8)
        var gen = UInt32(b[8]) | UInt32(b[9]) << 8 | UInt32(b[10]) << 16 | UInt32(b[11]) << 24
        gen &+= 1
        for k in 0..<4 { b[8 + k] = UInt8((gen >> (8 * UInt32(k))) & 0xff) }
        let fm = FileManager.default
        let bak = database.appendingPathExtension("bak")
        try? fm.removeItem(at: bak); try? fm.copyItem(at: database, to: bak)
        try Data(b).write(to: database, options: .atomic)
    }
    /// Copies the database to `dest` (a file, or a folder: MiiDatabase.dat goes inside).
    @discardableResult public func export(to dest: URL) throws -> URL {
        guard exists else { throw CLIError.failed(code: 1, message: "There is no Mii database yet. Start a game once.") }
        var isDir: ObjCBool = false
        let target = FileManager.default.fileExists(atPath: dest.path, isDirectory: &isDir) && isDir.boolValue ? dest.appendingPathComponent("MiiDatabase.dat") : dest
        try? FileManager.default.removeItem(at: target)
        try FileManager.default.copyItem(at: database, to: target)
        return target
    }
    /// Backup next to the database with a timestamp.
    @discardableResult public func backup(now: Date = Date()) throws -> URL {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; f.locale = Locale(identifier: "en_US_POSIX")
        return try export(to: folder.appendingPathComponent("MiiDatabase-\(f.string(from: now)).dat"))
    }
    /// Replaces the database with a valid file (a backup).
    public func restore(from src: URL) throws {
        guard let d = try? Data(contentsOf: src), validate(d) != nil else { throw CLIError.badOutput("not a Napple Mii database") }
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: database.appendingPathExtension("bak"))
        if exists { try? FileManager.default.copyItem(at: database, to: database.appendingPathExtension("bak")) }
        try d.write(to: database, options: .atomic)
    }
    func validate(_ d: Data) -> Int? {
        let b = [UInt8](d)
        guard b.count >= Self.headerSize, b[0..<4].elementsEqual("NFDB".utf8), b[4] == 1, b.count == Self.headerSize + Int(b[5]) * Self.entrySize else { return nil }
        let crc = Self.crc16(b[Self.headerSize...])
        return b[6] == UInt8(crc & 0xff) && b[7] == UInt8(crc >> 8) ? Int(b[5]) : nil
    }
}

/// Runs a built game app and streams its log (the runtime prints its log on stderr) for the per-game Run view.
public final class GameRun: @unchecked Sendable {
    public let app: URL
    private var process: Process?
    private let lock = NSLock()
    public init(app: URL) { self.app = app }

    /// The executable of a built .app (CFBundleExecutable; the launcher).
    public static func executable(of app: URL) -> URL? {
        guard let d = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")), let e = d["CFBundleExecutable"] as? String else { return nil }
        let u = app.appendingPathComponent("Contents/MacOS/\(e)")
        return FileManager.default.isExecutableFile(atPath: u.path) ? u : nil
    }
    public var isRunning: Bool { lock.lock(); defer { lock.unlock() }; return process?.isRunning ?? false }

    /// Starts the game; `onLine` gets every stdout/stderr line; `onExit` the status. Environment additions are explicit (system.json is read by the runtime itself).
    public func start(env: [String: String] = [:], onLine: @escaping @Sendable (String) -> Void, onExit: @escaping @Sendable (Int32) -> Void) throws {
        guard let exe = Self.executable(of: app) else { throw CLIError.failed(code: 127, message: "\(app.lastPathComponent) has no launcher.") }
        let p = Process()
        p.executableURL = exe
        p.environment = ProcessInfo.processInfo.environment.merging(env) { _, n in n }
        let o = Pipe(), e = Pipe()
        p.standardOutput = o; p.standardError = e; p.standardInput = FileHandle.nullDevice
        lock.lock(); process = p; lock.unlock()
        p.terminationHandler = { q in onExit(q.terminationStatus) }
        try p.run()
        Task.detached { await NappleCLI.lines(of: o.fileHandleForReading, onLine) }
        Task.detached { await NappleCLI.lines(of: e.fileHandleForReading, onLine) }
    }
    public func stop() { lock.lock(); let p = process; lock.unlock(); if p?.isRunning == true { p?.terminate() } }
}
