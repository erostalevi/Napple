import Foundation

/// Whether the Apple account behind a signing identity is a free (Personal Team) or paid Apple Developer Program account.
public enum AccountKind: String, Equatable, Sendable {
    case paid, free, unknown

    public var label: String {
        switch self {
        case .paid: return "Apple Developer Program"
        case .free: return "Free Apple ID (Personal Team)"
        case .unknown: return "Unknown account type"
        }
    }
}

/// The parts of a decoded `.mobileprovision` Studio cares about.
public struct ProfileDetails: Equatable, Sendable {
    public var name: String
    public var teamId: String
    public var teamName: String
    public var appId: String                 // application-identifier, `TEAMID.com.example.app`
    public var created: Date?
    public var expires: Date?
    public var xcodeManaged: Bool
    public var provisionsAllDevices: Bool
    public var deviceCount: Int
    public var entitlements: [String: Bool]  // boolean entitlements only (get-task-allow, increased-memory-limit, ...)
    public var certificates: [Data] = []     // DeveloperCertificates (DER)

    public static let increasedMemory = "com.apple.developer.kernel.increased-memory-limit"
    public static let increasedDebuggableMemory = "com.apple.developer.kernel.increased-debuggable-memory-limit"

    public init(name: String = "", teamId: String = "", teamName: String = "", appId: String = "", created: Date? = nil, expires: Date? = nil,
                xcodeManaged: Bool = false, provisionsAllDevices: Bool = false, deviceCount: Int = 0, entitlements: [String: Bool] = [:]) {
        self.name = name; self.teamId = teamId; self.teamName = teamName; self.appId = appId; self.created = created; self.expires = expires
        self.xcodeManaged = xcodeManaged; self.provisionsAllDevices = provisionsAllDevices; self.deviceCount = deviceCount; self.entitlements = entitlements
    }

    public var grantsIncreasedMemory: Bool { entitlements[Self.increasedMemory] == true }
    public var grantsDebuggableMemory: Bool { entitlements[Self.increasedDebuggableMemory] == true }
    public var lifetimeDays: Double? {
        guard let c = created, let e = expires else { return nil }
        return e.timeIntervalSince(c) / 86_400
    }

    /// Parses the plist inside a profile (already CMS-decoded).
    public static func parse(plist: [String: Any]) -> ProfileDetails {
        let ents = plist["Entitlements"] as? [String: Any] ?? [:]
        var bools: [String: Bool] = [:]
        for (k, v) in ents { if let b = v as? Bool { bools[k] = b } }
        let team = (plist["TeamIdentifier"] as? [String])?.first ?? (ents["com.apple.developer.team-identifier"] as? String) ?? ""
        var p = ProfileDetails(name: plist["Name"] as? String ?? "", teamId: team, teamName: plist["TeamName"] as? String ?? "",
                               appId: ents["application-identifier"] as? String ?? "", created: plist["CreationDate"] as? Date,
                               expires: plist["ExpirationDate"] as? Date, xcodeManaged: plist["IsXcodeManaged"] as? Bool ?? false,
                               provisionsAllDevices: plist["ProvisionsAllDevices"] as? Bool ?? false,
                               deviceCount: (plist["ProvisionedDevices"] as? [Any])?.count ?? 0, entitlements: bools)
        p.certificates = plist["DeveloperCertificates"] as? [Data] ?? []
        return p
    }

    /// Decodes a `.mobileprovision` file with `security cms -D`.
    public static func read(_ file: URL) -> ProfileDetails? {
        guard let out = try? ToolRunner.system.runSync(URL(fileURLWithPath: "/usr/bin/security"), ["cms", "-D", "-i", file.path]),
              let data = out.stdout.data(using: .utf8),
              let plist = (try? PropertyListSerialization.propertyList(from: data, format: nil)) as? [String: Any] else { return nil }
        return parse(plist: plist)
    }
}

/// What Xcode knows about the teams signed in on this Mac (`com.apple.dt.Xcode` IDEProvisioningTeamByIdentifier).
public struct XcodeTeam: Equatable, Sendable {
    public var teamId: String
    public var name: String
    public var type: String
    public var isFree: Bool
    public init(teamId: String, name: String, type: String, isFree: Bool) { self.teamId = teamId; self.name = name; self.type = type; self.isFree = isFree }

    /// Parses the preferences dictionary (account id -> [team dictionaries]).
    public static func parse(_ prefs: [String: Any]) -> [XcodeTeam] {
        var seen = Set<String>(), out: [XcodeTeam] = []
        for (_, v) in prefs {
            for t in v as? [[String: Any]] ?? [] {
                guard let id = t["teamID"] as? String, seen.insert(id).inserted else { continue }
                let type = t["teamType"] as? String ?? ""
                let free = (t["isFreeProvisioningTeam"] as? Bool) ?? ((t["isFreeProvisioningTeam"] as? Int) == 1) || type == "Personal Team"
                out.append(XcodeTeam(teamId: id, name: t["teamName"] as? String ?? id, type: type, isFree: free))
            }
        }
        return out.sorted { $0.teamId < $1.teamId }
    }

    public static func load() -> [XcodeTeam] {
        let v = CFPreferencesCopyAppValue("IDEProvisioningTeamByIdentifier" as CFString, "com.apple.dt.Xcode" as CFString)
        return parse(v as? [String: Any] ?? [:])
    }
}

public enum AccountDetector {
    /// Decides free versus paid, strongest evidence first:
    /// 1. Xcode's own team list says Personal Team / free provisioning team;
    /// 2. a profile that grants the increased-memory capability can only come from a paid team;
    /// 3. profile lifetime: free Apple IDs get 7-day profiles, the paid program gets one year (or 90+ days for development);
    /// 4. a team name ending in "(Personal Team)".
    /// Anything else is `.unknown`, which Studio treats like free (it never adds an entitlement it cannot prove).
    public static func classify(profile: ProfileDetails?, teams: [XcodeTeam]) -> AccountKind {
        if let p = profile, let t = teams.first(where: { $0.teamId == p.teamId }) { return t.isFree ? .free : .paid }
        guard let p = profile else { return .unknown }
        if p.grantsIncreasedMemory { return .paid }
        if let d = p.lifetimeDays { if d <= 8 { return .free }; if d >= 60 { return .paid } }
        if p.teamName.hasSuffix("(Personal Team)") { return .free }
        return .unknown
    }
}

/// Entitlements Studio adds on top of the profile's, and what to tell the user about it.
public struct IOSEntitlementPlan: Equatable, Sendable {
    public var entitlements: [String: Bool]   // only the keys Studio adds
    public var includesMemory: Bool
    public var explanation: String
    /// Set when the entitlement is wanted but the profile (App ID) does not carry the capability yet.
    public var needsCapability: Bool

    /// Rules (no JIT, no private API, nothing else is ever added):
    /// - paid account: `increased-memory-limit` always, `increased-debuggable-memory-limit` for debug builds;
    ///   when a fixed profile is supplied that lacks the capability the plan says so, and automatic signing (xcodebuild
    ///   -allowProvisioningUpdates) is what turns the capability on for the App ID;
    /// - free or unknown account: nothing is added, the build is normal and the explanation says why.
    public static func make(account: AccountKind, debug: Bool, profile: ProfileDetails? = nil) -> IOSEntitlementPlan {
        switch account {
        case .paid:
            var e = [ProfileDetails.increasedMemory: true]
            if debug { e[ProfileDetails.increasedDebuggableMemory] = true }
            let missing = profile.map { !$0.grantsIncreasedMemory } ?? false
            let why = missing
                ? "Your account is a paid developer account, so Napple asks for the Increased Memory Limit capability. The selected provisioning profile does not carry it yet: use automatic signing so Xcode adds the capability to the App ID, or enable it in your Apple Developer account and regenerate the profile."
                : "Your account is a paid developer account, so the app asks iOS for a higher memory limit (Increased Memory Limit). Big games need it."
            return IOSEntitlementPlan(entitlements: e, includesMemory: true, explanation: why, needsCapability: missing)
        case .free:
            return IOSEntitlementPlan(entitlements: [:], includesMemory: false,
                explanation: "Free Apple IDs cannot use the Increased Memory Limit capability, so the app is built without it and runs with the normal iOS memory limit. Large games may be closed by iOS; a paid Apple Developer Program account lifts that.",
                needsCapability: false)
        case .unknown:
            return IOSEntitlementPlan(entitlements: [:], includesMemory: false,
                explanation: "Studio could not tell whether this is a paid developer account, so it builds without the Increased Memory Limit capability. Sign in to Xcode with the account (Settings > Accounts) and refresh.",
                needsCapability: false)
        }
    }

    /// The `.entitlements` plist Studio hands to codesign for the ad-hoc stage (profile entitlements are merged by export).
    public func plist(base: [String: Any] = [:]) -> Data {
        var d = base
        for (k, v) in entitlements { d[k] = v }
        return (try? PropertyListSerialization.data(fromPropertyList: d, format: .xml, options: 0)) ?? Data()
    }
}

extension AccountDetector {
    /// Account kind from the team alone (automatic signing: there is no profile yet).
    public static func classify(team: XcodeTeam?) -> AccountKind {
        guard let team else { return .unknown }
        return team.isFree ? .free : .paid
    }
}
