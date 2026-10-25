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

/// One parsed `PROGRESS <stage> <percent> <message>` line (see docs/studio-protocol.md).
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
        let isIOS = BuildTarget(rawValue: r.target)?.isIOS ?? false
        // iOS apps have no launcher (no exec on iOS) and use the runtime built by tools/build_ios.sh
        let helpers = isIOS ? [("--runtime", "napple-runtime-ios")] : [("--runtime", "napple-runtime"), ("--launcher", "napple-launcher")]
        for (flag, file) in helpers {
            let u = dir.appendingPathComponent(file)
            if FileManager.default.isExecutableFile(atPath: u.path) { args += [flag, u.path] }
        }
        if isIOS, let o = r.ios {
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
        try await withTaskCancellationHandler {
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
