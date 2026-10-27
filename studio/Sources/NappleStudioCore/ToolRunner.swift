import Foundation

public struct ToolOutput: Equatable, Sendable {
    public var stdout: String
    public var stderr: String
    public var status: Int32
}

/// Runs command line tools (xcrun, codesign, ditto, ...). Tests swap in a recording fake.
public struct ToolRunner: Sendable {
    /// (executable, arguments, onLine) -> output; throws CLIError.failed on a non-zero exit.
    public typealias Handler = @Sendable (URL, [String], (@Sendable (String) -> Void)?) async throws -> ToolOutput
    public let handler: Handler
    public init(_ handler: @escaping Handler) { self.handler = handler }

    public func run(_ exe: URL, _ args: [String], onLine: (@Sendable (String) -> Void)? = nil) async throws -> ToolOutput {
        try await handler(exe, args, onLine)
    }

    /// xcrun <tool> ...
    public func xcrun(_ args: [String], onLine: (@Sendable (String) -> Void)? = nil) async throws -> ToolOutput {
        try await run(URL(fileURLWithPath: "/usr/bin/xcrun"), args, onLine: onLine)
    }

    public static let system = ToolRunner { exe, args, onLine in
        let p = Process()
        p.executableURL = exe; p.arguments = args
        let o = Pipe(), e = Pipe()
        p.standardOutput = o; p.standardError = e; p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { throw CLIError.failed(code: 127, message: "Could not run \(exe.path): \(error.localizedDescription). Install Xcode and its command line tools.") }
        let out = LockedBox<[String]>([]), err = LockedBox<[String]>([])
        await withTaskCancellationHandler {
            await withTaskGroup(of: Void.self) { g in
                g.addTask { await NappleCLI.lines(of: o.fileHandleForReading) { out.append($0); onLine?($0) } }
                g.addTask { await NappleCLI.lines(of: e.fileHandleForReading) { err.append($0); onLine?($0) } }
            }
            p.waitUntilExit()
        } onCancel: { p.terminate() }
        let r = ToolOutput(stdout: out.value.joined(separator: "\n"), stderr: err.value.joined(separator: "\n"), status: p.terminationStatus)
        if r.status != 0 {
            let msg = (r.stderr.isEmpty ? r.stdout : r.stderr).split(separator: "\n").suffix(4).joined(separator: " ")
            throw CLIError.failed(code: r.status, message: msg.isEmpty ? "\(exe.lastPathComponent) exited with code \(r.status)" : msg)
        }
        return r
    }

    /// Blocking variant for small, quick tools (security cms).
    public func runSync(_ exe: URL, _ args: [String]) throws -> ToolOutput {
        let p = Process()
        p.executableURL = exe; p.arguments = args
        let o = Pipe()
        p.standardOutput = o; p.standardError = FileHandle.nullDevice; p.standardInput = FileHandle.nullDevice
        try p.run()
        let data = o.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { throw CLIError.failed(code: p.terminationStatus, message: "\(exe.lastPathComponent) failed") }
        return ToolOutput(stdout: String(decoding: data, as: UTF8.self), stderr: "", status: 0)
    }
}
