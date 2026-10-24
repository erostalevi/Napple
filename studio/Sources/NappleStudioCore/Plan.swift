import Foundation

public struct PlanItem: Decodable, Identifiable, Equatable, Sendable {
    public let titleId: String
    public let version: UInt32
    public let versionLabel: String
    public let size: UInt64
    public var selected: Bool
    public let path: String
    public var id: String { "\(titleId)-\(version)" }

    enum CodingKeys: String, CodingKey {
        case titleId = "title_id", version, versionLabel = "version_label", size, selected, path
    }
    public init(titleId: String, version: UInt32, versionLabel: String, size: UInt64, selected: Bool, path: String) {
        self.titleId = titleId; self.version = version; self.versionLabel = versionLabel
        self.size = size; self.selected = selected; self.path = path
    }
}

/// Output of `napple plan ... --json`.
public struct PlanResult: Decodable, Equatable, Sendable {
    public var base: PlanItem?
    public var updates: [PlanItem]
    public var dlc: [PlanItem]
    public var warnings: [String]

    public init(base: PlanItem?, updates: [PlanItem], dlc: [PlanItem], warnings: [String]) {
        self.base = base; self.updates = updates; self.dlc = dlc; self.warnings = warnings
    }
    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: K.self)
        base = try c.decodeIfPresent(PlanItem.self, forKey: .base)
        updates = try c.decodeIfPresent([PlanItem].self, forKey: .updates) ?? []
        dlc = try c.decodeIfPresent([PlanItem].self, forKey: .dlc) ?? []
        warnings = try c.decodeIfPresent([String].self, forKey: .warnings) ?? []
    }
    enum K: String, CodingKey { case base, updates, dlc, warnings }

    public static func decode(_ data: Data) throws -> PlanResult {
        try JSONDecoder().decode(PlanResult.self, from: data)
    }

    /// Bytes added by embedding the selected updates and DLC (all are embedded; toggled at runtime).
    public var extraSelectedBytes: UInt64 {
        (updates + dlc).filter(\.selected).reduce(0) { $0 + $1.size }
    }
    public var totalSelectedBytes: UInt64 { (base?.size ?? 0) + extraSelectedBytes }

    public static func formatBytes(_ n: UInt64) -> String {
        let f = ByteCountFormatter()
        f.countStyle = .file
        return f.string(fromByteCount: Int64(clamping: n))
    }
}
