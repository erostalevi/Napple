import SwiftUI
import AppKit
import NappleStudioCore

/// "Play with Debug": the game runs as its own process and this object keeps its live log for the console window.
@MainActor
@Observable
final class DebugConsole: Identifiable {
    enum Level: String, CaseIterable, Identifiable { case all, warnings, errors; var id: String { rawValue } }

    let id = UUID()
    let name: String
    let app: URL
    private(set) var lines: [String] = []
    private(set) var running = false
    private(set) var exitCode: Int32?
    private(set) var startedAt: Date?
    private(set) var failure: String?
    private let buffer = SyncBox<[String]>([])
    @ObservationIgnored private var run: GameRun?
    @ObservationIgnored private var flushScheduled = false
    static let maxLines = 20_000

    init(name: String, app: URL) { self.name = name; self.app = app }

    /// Starts the game. Calling it again while it runs does nothing.
    func start() {
        guard !running else { return }
        lines.removeAll(); exitCode = nil; failure = nil
        let r = GameRun(app: app)
        run = r
        do {
            try r.start(onLine: { [weak self] l in self?.push(l) },
                        onExit: { [weak self] code in Task { @MainActor in self?.finished(code) } })
            running = true; startedAt = Date()
        } catch {
            failure = error.localizedDescription
            run = nil
        }
    }

    func stop() { run?.stop() }
    func clear() { lines.removeAll() }

    private nonisolated func push(_ line: String) {
        buffer.append(line)
        Task { @MainActor in self.scheduleFlush() }
    }

    private func scheduleFlush() {
        guard !flushScheduled else { return }
        flushScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.flush() }
    }

    private func flush() {
        flushScheduled = false
        let new = buffer.drain()
        guard !new.isEmpty else { return }
        lines.append(contentsOf: new)
        if lines.count > Self.maxLines { lines.removeFirst(lines.count - Self.maxLines / 2) }
    }

    private func finished(_ code: Int32) {
        flush()
        running = false; exitCode = code
    }

    // MARK: reading

    func shown(level: Level, filter: String) -> [(offset: Int, line: String)] {
        let q = filter.trimmingCharacters(in: .whitespaces)
        return lines.enumerated().compactMap { i, l in
            let lv = LogLevel.classify(l)
            switch level {
            case .all: break
            case .warnings: guard lv >= .warning else { return nil }
            case .errors: guard lv == .error else { return nil }
            }
            if !q.isEmpty, !l.localizedCaseInsensitiveContains(q) { return nil }
            return (i, l)
        }
    }

    var statusText: String {
        if let failure { return failure }
        if running { return String(localized: "Running") }
        if let c = exitCode { return c == 0 ? String(localized: "Exited normally") : String(localized: "Exited with code \(c)") }
        return String(localized: "Starting…")
    }

    func save(to url: URL) throws { try lines.joined(separator: "\n").appending("\n").write(to: url, atomically: true, encoding: .utf8) }
}

/// A tiny lock-protected value for callbacks that arrive on background threads.
final class SyncBox<T>: @unchecked Sendable {
    private var value: T
    private let lock = NSLock()
    init(_ v: T) { value = v }
    func with<R>(_ f: (inout T) -> R) -> R { lock.lock(); defer { lock.unlock() }; return f(&value) }
}
extension SyncBox where T == [String] {
    func append(_ s: String) { with { $0.append(s) } }
    func drain() -> [String] { with { let v = $0; $0.removeAll(keepingCapacity: true); return v } }
}
