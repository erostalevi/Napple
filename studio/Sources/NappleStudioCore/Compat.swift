import Foundation

public enum CompatStatus: Int, Comparable, Sendable {
    case perfect = 0, playable, partial, menus, boots, nothing, unknown
    public static func < (a: Self, b: Self) -> Bool { a.rawValue < b.rawValue }

    public init(label: String) {
        switch label.lowercased().trimmingCharacters(in: .whitespaces) {
        case "perfect": self = .perfect
        case "playable", "great", "good": self = .playable
        case "partial", "ingame", "in-game", "in game": self = .partial
        case "menus", "menu", "intro": self = .menus
        case "boots", "loads": self = .boots
        case "nothing", "broken", "wont boot", "won't boot": self = .nothing
        default: self = .unknown
        }
    }
    /// "At least Partial" is the bar below which Studio warns.
    public var isAtLeastPartial: Bool { self <= .partial }
    public var label: String {
        ["Perfect", "Playable", "Partial", "Menus only", "Boots only", "Does not work", "Unknown"][rawValue]
    }
}

public struct CompatStage: Equatable, Sendable {
    public var name: String
    public var status: String   // pass | partial | fail | blocked | untested
    public var reason: String?
}

public struct CompatEntry: Equatable, Sendable {
    public var titleId: String
    public var name: String?
    public var status: CompatStatus
    public var notes: String?
    public var stages: [CompatStage] = []

    /// Overall status derived from stage results when the file has no explicit status.
    public static func derive(_ stages: [CompatStage]) -> CompatStatus {
        func st(_ n: String) -> String? { stages.first { $0.name == n }?.status.lowercased() }
        switch st("ingame") {
        case "pass": return .playable
        case "partial": return .partial
        default: break
        }
        if st("title-screen") == "pass" { return .menus }
        if st("runtime-boot") == "pass" || st("runtime-boot") == "partial" { return .boots }
        if stages.contains(where: { ["fail", "blocked"].contains($0.status.lowercased()) }) { return .nothing }
        return .unknown
    }

    public static let stageOrder = ["ingest", "build-plan", "nso-load", "site-scan", "rewrite", "rewrite-verify", "runtime-boot",
                                    "video", "audio", "input", "saves", "title-screen", "ingame"]
}

/// Tolerant reader for compat/matrix.json. Accepts an array, {"games":[...]}, {"entries":[...]} or a
/// dictionary keyed by title id. Field names: title_id|titleId|id, name|title, status, notes|note|summary.
public struct CompatMatrix: Sendable {
    public var entries: [String: CompatEntry] = [:]
    public init(entries: [String: CompatEntry] = [:]) { self.entries = entries }

    public static func load(from url: URL) -> CompatMatrix? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return parse(data)
    }

    public static func parse(_ data: Data) -> CompatMatrix? {
        guard let root = try? JSONSerialization.jsonObject(with: data) else { return nil }
        var m = CompatMatrix()
        func add(_ o: [String: Any], key: String?) {
            let tid = (str(o, ["title_id", "titleId", "id", "tid"]) ?? key ?? "").uppercased()
            guard !tid.isEmpty else { return }
            var st = CompatStatus.unknown
            if let s = str(o, ["status", "state", "rating", "level"]) { st = CompatStatus(label: s) }
            else if let versions = o["versions"] as? [[String: Any]] {
                // No overall status in the file: derive the best one from the per-stage results.
                st = versions.map { derive(stages: $0["stages"] as? [String: Any] ?? [:]) }.min() ?? .unknown
            }
            var stages: [CompatStage] = []
            if let vs = o["versions"] as? [[String: Any]] {
                let v = vs.first { ($0["kind"] as? String) == "base" } ?? vs.first
                if let sd = v?["stages"] as? [String: Any] {
                    for (n, any) in sd {
                        if let so = any as? [String: Any] {
                            stages.append(CompatStage(name: n, status: str(so, ["status"]) ?? "untested", reason: str(so, ["reason"])))
                        }
                    }
                    stages.sort { (CompatEntry.stageOrder.firstIndex(of: $0.name) ?? 99) < (CompatEntry.stageOrder.firstIndex(of: $1.name) ?? 99) }
                }
            }
            if st == .unknown, !stages.isEmpty { st = CompatEntry.derive(stages) }
            let notes = str(o, ["summary", "description", "note"]) ?? (o["notes"] as? String)
            m.entries[tid] = CompatEntry(titleId: tid, name: str(o, ["name", "title"]), status: st, notes: notes, stages: stages)
        }
        func walk(_ any: Any) {
            if let arr = any as? [Any] { for case let o as [String: Any] in arr { add(o, key: nil) } }
            else if let d = any as? [String: Any] {
                for k in ["games", "entries", "titles"] { if let inner = d[k] { walk(inner); return } }
                for (k, v) in d { if let o = v as? [String: Any] { add(o, key: k) } else if let s = v as? String {
                    add(["status": s], key: k) } }
            }
        }
        walk(root)
        return m
    }

    /// Maps pipeline stage results to a user-facing status. Untested runtime stages stay Unknown.
    static func derive(stages: [String: Any]) -> CompatStatus {
        func s(_ k: String) -> String { ((stages[k] as? [String: Any])?["status"] as? String)?.lowercased() ?? "untested" }
        let ok: (String) -> Bool = { $0 == "pass" }
        let some: (String) -> Bool = { $0 == "pass" || $0 == "partial" }
        if ok(s("ingame")) { return .playable }
        if some(s("ingame")) { return .partial }
        if some(s("title-screen")) { return .menus }
        if some(s("runtime-boot")) { return .boots }
        if s("runtime-boot") == "fail" || s("runtime-boot") == "blocked" { return .nothing }
        return .unknown
    }

    public func entry(for titleId: String) -> CompatEntry? { entries[titleId.uppercased()] }

    private static func str(_ o: [String: Any], _ keys: [String]) -> String? {
        for k in keys { if let s = o[k] as? String, !s.isEmpty { return s } else if let n = o[k] as? NSNumber { return n.stringValue } }
        return nil
    }
}
