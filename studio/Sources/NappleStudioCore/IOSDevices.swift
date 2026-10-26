import Foundation

/// Build targets of `napple build --target` (docs/architecture/ios.md). iOS and iPadOS 26 or later only.
public enum BuildTarget: String, CaseIterable, Identifiable, Sendable {
    case macos, ios, iphone, ipad
    public var id: String { rawValue }
    public var label: String {
        switch self {
        case .macos: return "macOS"
        case .ios: return "iPhone & iPad"
        case .iphone: return "iPhone"
        case .ipad: return "iPad"
        }
    }
    public var isIOS: Bool { self != .macos }
}

/// A device `xcrun devicectl list devices` knows about.
public struct IOSDevice: Identifiable, Equatable, Sendable {
    public var id: String          // CoreDevice identifier (accepted by `devicectl --device`)
    public var name: String
    public var model: String
    public var osVersion: String
    public var connected: Bool     // tunnel is up (cable or trusted Wi-Fi)
    public var developerMode: Bool
    public var supported: Bool     // iOS / iPadOS 26 or later
    public init(id: String, name: String, model: String, osVersion: String, connected: Bool, developerMode: Bool) {
        self.id = id; self.name = name; self.model = model; self.osVersion = osVersion; self.connected = connected; self.developerMode = developerMode
        self.supported = (Int(osVersion.split(separator: ".").first ?? "") ?? 0) >= 26
    }
    public var summary: String {
        var s = "\(name) (\(model), \(osVersion))"
        if !connected { s += " - not connected" } else if !developerMode { s += " - Developer Mode is off" } else if !supported { s += " - needs iOS 26" }
        return s
    }
}

public enum DeviceList {
    /// Parses the file written by `xcrun devicectl list devices --json-output <file>`. Phones and tablets only; unknown shapes yield [].
    public static func parse(_ data: Data) -> [IOSDevice] {
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let result = root["result"] as? [String: Any], let devices = result["devices"] as? [[String: Any]] else { return [] }
        return devices.compactMap { d in
            guard let id = d["identifier"] as? String else { return nil }
            let props = d["deviceProperties"] as? [String: Any] ?? [:]
            let hw = d["hardwareProperties"] as? [String: Any] ?? [:]
            let conn = d["connectionProperties"] as? [String: Any] ?? [:]
            let platform = (hw["platform"] as? String ?? "").lowercased()
            let family = (hw["deviceType"] as? String ?? "").lowercased()
            guard platform == "ios" || platform == "ipados" || family == "iphone" || family == "ipad" else { return nil }
            let tunnel = (conn["tunnelState"] as? String ?? "").lowercased()
            return IOSDevice(id: id, name: props["name"] as? String ?? "Unnamed device",
                             model: hw["marketingName"] as? String ?? hw["productType"] as? String ?? "iOS device",
                             osVersion: props["osVersionNumber"] as? String ?? "?",
                             connected: tunnel == "connected", developerMode: (props["developerModeStatus"] as? String ?? "") == "enabled")
        }
    }
}

/// `napple build --list-signing --json`
public struct SigningIdentity: Decodable, Identifiable, Equatable, Sendable, Hashable {
    public var sha1: String
    public var name: String
    public var id: String { sha1 }
}
public struct SigningProfile: Decodable, Identifiable, Equatable, Sendable, Hashable {
    public var path: String
    public var name: String
    public var team: String
    public var appId: String
    public var expires: String
    public var devices: Int
    public var id: String { path }
    enum CodingKeys: String, CodingKey { case path, name, team, appId = "app_id", expires, devices }

    /// Does this profile cover the bundle id? (`TEAMID.com.example.app`, `TEAMID.*`, `TEAMID.com.example.*`; an empty id only fits wildcards.)
    public func covers(bundleId: String) -> Bool {
        guard let dot = appId.firstIndex(of: ".") else { return false }
        let pat = String(appId[appId.index(after: dot)...])
        if pat.hasSuffix("*") { return bundleId.hasPrefix(String(pat.dropLast())) }
        return pat == bundleId
    }
    /// The explicit bundle id of a non-wildcard profile (what a free Apple ID profile is bound to), else nil.
    public var explicitBundleId: String? {
        guard let dot = appId.firstIndex(of: ".") else { return nil }
        let pat = String(appId[appId.index(after: dot)...])
        return pat.contains("*") ? nil : pat
    }
}
public struct SigningInfo: Decodable, Equatable, Sendable {
    public var identities: [SigningIdentity]
    public var profiles: [SigningProfile]
    public static func decode(_ data: Data) throws -> SigningInfo { try JSONDecoder().decode(SigningInfo.self, from: data) }
}

public struct IOSOptions: Sendable, Equatable {
    public var identity: String?
    public var profile: URL?
    public var bundleId: String?
    public var ipa: Bool
    /// Build an unsigned app/IPA for an external signer (AltStore, Sideloadly, ...).
    public var unsigned: Bool
    public init(identity: String? = nil, profile: URL? = nil, bundleId: String? = nil, ipa: Bool = false, unsigned: Bool = false) {
        self.identity = identity; self.profile = profile; self.bundleId = bundleId; self.ipa = ipa; self.unsigned = unsigned
    }
}
