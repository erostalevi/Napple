import Foundation

/// How serious a log line looks. Runtime and build lines carry no level field, so the words in them decide.
public enum LogLevel: Int, Comparable, Sendable {
    case info = 0, warning, error

    public static func < (a: LogLevel, b: LogLevel) -> Bool { a.rawValue < b.rawValue }

    public static func classify(_ line: String) -> LogLevel {
        let l = line.lowercased()
        if l.hasPrefix("error") || l.contains("error:") || l.contains("fatal") || l.contains("panic") || l.contains(" failed") || l.hasSuffix("failed") { return .error }
        if l.hasPrefix("warn") || l.contains("warning:") { return .warning }
        return .info
    }
}
