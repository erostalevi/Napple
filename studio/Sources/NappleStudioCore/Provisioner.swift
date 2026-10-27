import Foundation
import CryptoKit

public enum SigningMode: String, CaseIterable, Identifiable, Sendable {
    /// Xcode creates the profile (and turns the capabilities on) through a stub project; `napple build` signs with it.
    case automatic
    /// A provisioning profile chosen by hand, signed by `napple build` itself.
    case profile
    /// Unsigned app/IPA for an external signer.
    case unsigned
    public var id: String { rawValue }
    public var label: String {
        switch self { case .automatic: return "Automatic (Xcode)"; case .profile: return "Provisioning profile"; case .unsigned: return "Unsigned (external signer)" }
    }
}

/// Automatic signing without an Xcode project of our own: a throwaway stub project with the game's bundle id, team and entitlements is built once with
/// `xcodebuild -allowProvisioningUpdates -allowProvisioningDeviceRegistration`, which makes Xcode register the App ID, turn on its capabilities
/// (Increased Memory Limit for paid teams), register the connected device and download a development profile. `napple build` then signs with that
/// profile and the matching certificate, exactly like the manual profile mode. (`xcodebuild -exportArchive` refuses archives it did not build itself:
/// "certificateType DEVELOPMENT_MANAGED has an invalid value".)
public struct Provisioner: Sendable {
    public let tools: ToolRunner
    public let profilesDir: URL
    public init(tools: ToolRunner = .system,
                profilesDir: URL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Developer/Xcode/UserData/Provisioning Profiles")) {
        self.tools = tools; self.profilesDir = profilesDir
    }

    public struct Result: Equatable, Sendable {
        public var profile: URL
        public var details: ProfileDetails
        public var identitySHA1: String?   // the certificate the profile was issued for, when it is in the keychain list
    }

    /// Writes `Stub.xcodeproj` + sources under `dir`. The entitlements plist carries exactly what the plan asks for (nothing for free accounts).
    public static func writeStubProject(in dir: URL, teamId: String, bundleId: String, entitlements: IOSEntitlementPlan) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: dir.appendingPathComponent("Stub.xcodeproj"), withIntermediateDirectories: true)
        try fm.createDirectory(at: dir.appendingPathComponent("Stub"), withIntermediateDirectories: true)
        try "int main(void) { return 0; }\n".write(to: dir.appendingPathComponent("Stub/main.m"), atomically: true, encoding: .utf8)
        try entitlements.plist().write(to: dir.appendingPathComponent("Stub/Stub.entitlements"))
        try pbxproj(teamId: teamId, bundleId: bundleId).write(to: dir.appendingPathComponent("Stub.xcodeproj/project.pbxproj"), atomically: true, encoding: .utf8)
    }

    public static func pbxproj(teamId: String, bundleId: String) -> String {
        func q(_ s: String) -> String { "\"" + s.replacingOccurrences(of: "\"", with: "") + "\"" }
        return """
        // !$*UTF8*$!
        {
        \tarchiveVersion = 1;
        \tclasses = {};
        \tobjectVersion = 56;
        \tobjects = {
        \t\tA1 /* main.m */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; path = main.m; sourceTree = "<group>"; };
        \t\tA2 /* Stub.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Stub.app; sourceTree = BUILT_PRODUCTS_DIR; };
        \t\tA3 /* Stub.entitlements */ = {isa = PBXFileReference; lastKnownFileType = text.plist.entitlements; path = Stub.entitlements; sourceTree = "<group>"; };
        \t\tB1 /* main.m in Sources */ = {isa = PBXBuildFile; fileRef = A1; };
        \t\tC1 = {isa = PBXGroup; children = (C2, C3); sourceTree = "<group>"; };
        \t\tC2 /* Stub */ = {isa = PBXGroup; children = (A1, A3); path = Stub; sourceTree = "<group>"; };
        \t\tC3 /* Products */ = {isa = PBXGroup; children = (A2); name = Products; sourceTree = "<group>"; };
        \t\tD1 /* Sources */ = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (B1); runOnlyForDeploymentPostprocessing = 0; };
        \t\tE1 /* Stub */ = {isa = PBXNativeTarget; buildConfigurationList = F1; buildPhases = (D1); buildRules = (); dependencies = (); name = Stub; productName = Stub; productReference = A2; productType = "com.apple.product-type.application"; };
        \t\tG1 /* Project object */ = {isa = PBXProject; attributes = { LastUpgradeCheck = 1500; TargetAttributes = { E1 = { CreatedOnToolsVersion = 15.0; }; }; }; buildConfigurationList = F2; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base); mainGroup = C1; productRefGroup = C3; projectDirPath = ""; projectRoot = ""; targets = (E1); };
        \t\tH1 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = { CODE_SIGN_ENTITLEMENTS = Stub/Stub.entitlements; CODE_SIGN_STYLE = Automatic; DEVELOPMENT_TEAM = \(q(teamId)); GENERATE_INFOPLIST_FILE = YES; INFOPLIST_KEY_CFBundleDisplayName = Stub; IPHONEOS_DEPLOYMENT_TARGET = 26.0; PRODUCT_BUNDLE_IDENTIFIER = \(q(bundleId)); PRODUCT_NAME = "$(TARGET_NAME)"; SDKROOT = iphoneos; TARGETED_DEVICE_FAMILY = "1,2"; }; name = Debug; };
        \t\tH2 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = { ALWAYS_SEARCH_USER_PATHS = NO; SDKROOT = iphoneos; }; name = Debug; };
        \t\tF1 = {isa = XCConfigurationList; buildConfigurations = (H1); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
        \t\tF2 = {isa = XCConfigurationList; buildConfigurations = (H2); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
        \t};
        \trootObject = G1;
        }

        """
    }

    /// The xcodebuild invocation (also shown in the UI).
    public static func commandLine(projectDir: URL) -> [String] {
        ["xcrun", "xcodebuild", "-project", projectDir.appendingPathComponent("Stub.xcodeproj").path, "-target", "Stub", "-configuration", "Debug",
         "-sdk", "iphoneos", "-destination", "generic/platform=iOS", "-allowProvisioningUpdates", "-allowProvisioningDeviceRegistration",
         "-derivedDataPath", projectDir.appendingPathComponent("DerivedData").path, "build"]
    }

    /// Runs the stub build and returns the profile Xcode produced for the bundle id (newest covering profile of that team).
    public func provision(teamId: String, bundleId: String, entitlements: IOSEntitlementPlan, work: URL, identities: [SigningIdentity],
                          onLine: (@Sendable (String) -> Void)? = nil) async throws -> Result {
        try? FileManager.default.removeItem(at: work)
        try Self.writeStubProject(in: work, teamId: teamId, bundleId: bundleId, entitlements: entitlements)
        let cmd = Self.commandLine(projectDir: work)
        do { _ = try await tools.xcrun(Array(cmd.dropFirst()), onLine: onLine) }
        catch { throw CLIError.failed(code: 1, message: Self.explain(error.localizedDescription)) }
        guard let found = Self.newestProfile(in: profilesDir, teamId: teamId, bundleId: bundleId) else {
            throw CLIError.failed(code: 1, message: String(localized: "Xcode finished but no provisioning profile for \(bundleId) appeared in \(profilesDir.path). Open the stub project in Xcode once (\(work.path)) to see what it asks for."))
        }
        let sha = found.details.certificateSHA1s.first { s in identities.contains { $0.sha1.caseInsensitiveCompare(s) == .orderedSame } }
        return Result(profile: found.url, details: found.details, identitySHA1: sha)
    }

    /// Newest `.mobileprovision` of the team whose application-identifier covers the bundle id.
    public static func newestProfile(in dir: URL, teamId: String, bundleId: String, read: (URL) -> ProfileDetails? = ProfileDetails.read) -> (url: URL, details: ProfileDetails)? {
        let files = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? [])
            .filter { $0.pathExtension == "mobileprovision" }
        var best: (URL, ProfileDetails)?
        for f in files {
            guard let d = read(f), d.teamId == teamId, SigningProfile(path: f.path, name: d.name, team: d.teamId, appId: d.appId, expires: "", devices: d.deviceCount).covers(bundleId: bundleId) else { continue }
            if let b = best, (b.1.created ?? .distantPast) >= (d.created ?? .distantPast) { continue }
            best = (f, d)
        }
        return best
    }

    public static func explain(_ raw: String) -> String {
        let l = raw.lowercased()
        if l.contains("no account for team") || l.contains("no accounts") || l.contains("sign in") {
            return String(localized: "Xcode has no account for this team. Open Xcode > Settings > Accounts, sign in with your Apple ID, then try again. (\(raw))")
        }
        if l.contains("no devices") || l.contains("register") && l.contains("device") {
            return String(localized: "Your team has no registered device yet. Connect the iPhone or iPad and unlock it so Xcode can register it, then try again. (\(raw))")
        }
        if l.contains("increased-memory") || l.contains("not available") && l.contains("capabilit") {
            return String(localized: "The account cannot use the Increased Memory Limit capability. A free Apple ID cannot; use a paid developer account. (\(raw))")
        }
        if l.contains("maximum number of") || l.contains("app id") && l.contains("limit") {
            return String(localized: "A free Apple ID may only create 10 App IDs per week; wait or reuse a bundle id prefix the team already has. (\(raw))")
        }
        return raw
    }
}

extension ProfileDetails {
    /// SHA-1 fingerprints (upper-case hex) of the `DeveloperCertificates` DER blobs, matching `security find-identity` / `napple build --list-signing`.
    public var certificateSHA1s: [String] { certificates.map { Insecure.SHA1.hash(data: $0).map { String(format: "%02X", $0) }.joined() } }
}

