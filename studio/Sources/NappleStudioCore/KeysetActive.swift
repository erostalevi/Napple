import Foundation

extension Keyset {
    /// When the set's folder was created (the order "the set created last is the active one" uses).
    public var createdAt: Date {
        (try? directory.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
    }
}

/// The rule behind "which key set is in use": the one created last; after the active one is deleted, the first remaining one.
public enum ActiveKeyset {
    /// The set created last (ties: the later name).
    public static func newest(in sets: [Keyset]) -> Keyset? {
        sets.max { a, b in
            let (x, y) = (a.createdAt, b.createdAt)
            return x != y ? x < y : a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
    }

    /// The name to use after a change: the remembered one while it still exists, else (nothing remembered) the newest set, else the first.
    public static func resolve(remembered: String?, in sets: [Keyset]) -> String {
        if let r = remembered, sets.contains(where: { $0.name == r }) { return r }
        if remembered == nil || remembered?.isEmpty == true { return newest(in: sets)?.name ?? "" }
        return sets.first?.name ?? ""
    }
}
