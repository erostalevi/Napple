import SwiftUI
import AppKit
import NappleStudioCore

extension AppModel {
    var selectedProfile: SigningProfile? { signing?.profiles.first { $0.path == iosProfilePath } }
    var selectedProfileDetails: ProfileDetails? { profileDetails[iosProfilePath] }
    var selectedDevice: IOSDevice? { devices.first { $0.id == iosDeviceId } }
    var selectedTeam: XcodeTeam? { xcodeTeams.first { $0.teamId == teamId } }

    var target: BuildTarget {
        switch destination {
        case .mac: .macos
        case .device: selectedDevice?.simulator == true ? .iosSim : .ios
        case .ipaIPhone: .iphone
        case .ipaIPad: .ipad
        }
    }
    var wantsIPA: Bool { destination == .ipaIPhone || destination == .ipaIPad }
    var destinationLabel: String {
        switch destination {
        case .mac: String(localized: "This Mac")
        case .device(let id): devices.first { $0.id == id }?.name ?? String(localized: "Selected device")
        case .ipaIPhone: String(localized: "iPhone .ipa")
        case .ipaIPad: String(localized: "iPad .ipa")
        }
    }

    /// Free versus paid, from the strongest evidence available for the chosen team or profile.
    var accountKind: AccountKind {
        switch signingMode {
        case .automatic: AccountDetector.classify(team: selectedTeam)
        case .profile: AccountDetector.classify(profile: selectedProfileDetails, teams: xcodeTeams)
        case .unsigned: .unknown
        }
    }
    var entitlementPlan: IOSEntitlementPlan {
        IOSEntitlementPlan.make(account: accountKind, debug: iosDebugBuild, profile: signingMode == .profile ? selectedProfileDetails : nil)
    }

    /// One bundle id per game (`<prefix>.<titleid>`), so several games can live on a device at once. A non-wildcard profile pins its own id instead.
    var effectiveBundleId: String {
        BundleIDs.make(prefix: iosBundlePrefix, titleId: plan?.base?.titleId ?? builtInfo?.titleId,
                       profileAppId: signingMode == .profile ? selectedProfile?.appId : nil)
    }
    /// Shown when the chosen profile forces one id for every game.
    var bundleIdNote: String? {
        guard signingMode == .profile, let p = selectedProfile, p.explicitBundleId != nil else { return nil }
        return String(localized: "This profile pins the bundle id \(p.explicitBundleId ?? ""), so only one game at a time can be installed with it. Use Automatic signing to get one id per game.")
    }

    var destinationBlocker: String? {
        guard destination.isIOS else { return nil }
        if cli == nil { return CLIError.notFound.localizedDescription }
        if case .device = destination {
            guard let dev = selectedDevice else { return String(localized: "That device is not connected any more. Choose another destination or reconnect it.") }
            if !dev.connected { return String(localized: "\(dev.name) is not connected. Plug it in and unlock it.") }
            if !dev.supported { return String(localized: "\(dev.name) runs iOS \(dev.osVersion); Napple apps need iOS or iPadOS 26 or later.") }
            if !dev.developerMode { return String(localized: "Turn on Developer Mode on \(dev.name): Settings > Privacy & Security > Developer Mode, then restart it.") }
            if signingMode == .unsigned, !dev.simulator { return String(localized: "Unsigned apps cannot be installed on a device. Choose a signing mode, or export an .ipa for an external signer.") }
            if dev.simulator { return nil }   // Simulator builds are ad-hoc signed: no team, profile or entitlements involved
        }
        switch signingMode {
        case .unsigned: return nil
        case .automatic:
            if xcodeTeams.isEmpty { return String(localized: "Xcode has no signed-in account. Open Xcode > Settings > Accounts and add your Apple ID, then refresh.") }
            if selectedTeam == nil { return String(localized: "Choose a team in Signing.") }
            if signing?.identities.isEmpty ?? true { return String(localized: "No Apple Development certificate on this Mac. In Xcode: Settings > Accounts > Manage Certificates > + Apple Development.") }
            return nil
        case .profile:
            if signing?.identities.isEmpty ?? true { return String(localized: "No code-signing identity found. In Xcode: Settings > Accounts > Manage Certificates > + Apple Development.") }
            if iosIdentity.isEmpty { return String(localized: "Choose a signing identity in Signing.") }
            guard let prof = selectedProfile else { return String(localized: "Choose a provisioning profile in Signing.") }
            return nil
        }
    }

    func iosOptions() -> IOSOptions? {
        guard target.isIOS else { return nil }
        if target.isSimulator { return IOSOptions(bundleId: effectiveBundleId) }
        switch signingMode {
        case .unsigned: return IOSOptions(bundleId: effectiveBundleId, ipa: wantsIPA, unsigned: true)
        case .profile: return IOSOptions(identity: iosIdentity, profile: selectedProfile.map { URL(fileURLWithPath: $0.path) }, bundleId: effectiveBundleId, ipa: wantsIPA)
        case .automatic:
            // filled in by provisionIfNeeded() right before the build (Xcode makes the profile; napple build signs with it)
            return IOSOptions(bundleId: effectiveBundleId, ipa: wantsIPA)
        }
    }

    /// Automatic mode: have Xcode create/refresh the profile for this bundle id (and the App ID capabilities), then sign like profile mode.
    func provisionIfNeeded(_ opts: IOSOptions?) async throws -> IOSOptions? {
        guard var o = opts, target.isIOS, !target.isSimulator, signingMode == .automatic, let team = selectedTeam else { return opts }
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("napple-provision-\(UUID().uuidString)")
        progress = ProgressEvent(stage: "provision", percent: 2, message: String(localized: "Asking Xcode for a provisioning profile (\(effectiveBundleId))"))
        appendLog("$ " + Provisioner.commandLine(projectDir: work).joined(separator: " "))
        let r = try await Provisioner(tools: tools).provision(teamId: team.teamId, bundleId: effectiveBundleId, entitlements: entitlementPlan, work: work,
                                                               identities: signing?.identities ?? []) { l in Task { @MainActor in self.appendLog(l) } }
        guard let sha = r.identitySHA1 ?? signing?.identities.first?.sha1 else { throw CLIError.failed(code: 1, message: String(localized: "No signing certificate matches the profile Xcode made.")) }
        o.identity = sha; o.profile = r.profile
        profileDetails[r.profile.path] = r.details
        notes.append(String(localized: "Signed with \(r.details.name) (\(r.details.teamName)), expires \(r.details.expires.map { $0.formatted(date: .abbreviated, time: .omitted) } ?? "?")."))
        if entitlementPlan.includesMemory, !r.details.grantsIncreasedMemory { notes.append(String(localized: "The profile Xcode made does not grant the Increased Memory Limit yet; check the App ID's capabilities in your developer account.")) }
        return o
    }

    /// After the CLI wrote (and signed) the app: pick up the .ipa and install when asked.
    func finishIOS(app: URL, plan: IOSEntitlementPlan, output: URL) async throws {
        if target.isSimulator {
            notes.append(String(localized: "Simulator build: ad-hoc signed, no memory entitlement (the Simulator has no memory limit)."))
        } else {
            if signingMode != .unsigned { notes.append(plan.explanation) }
            let ipa = app.deletingPathExtension().appendingPathExtension("ipa")
            if FileManager.default.fileExists(atPath: ipa.path) { builtIPA = ipa }
        }
        if case .device = destination, iosInstallAfterBuild, signingMode != .unsigned, selectedDevice != nil {
            await installToDevice(app)
        }
    }

    func refreshIOS() async {
        if let cli { signing = try? await cli.listSigning() }
        xcodeTeams = XcodeTeam.load()
        if let s = signing {
            if !s.identities.contains(where: { $0.sha1 == iosIdentity }) { iosIdentity = s.identities.first?.sha1 ?? "" }
            if !s.profiles.contains(where: { $0.path == iosProfilePath }) { iosProfilePath = s.profiles.first?.path ?? "" }
            let paths = s.profiles.map(\.path)
            let details = await Task.detached { Dictionary(uniqueKeysWithValues: paths.compactMap { p in ProfileDetails.read(URL(fileURLWithPath: p)).map { (p, $0) } }) }.value
            profileDetails = details
        }
        if !xcodeTeams.contains(where: { $0.teamId == teamId }) { teamId = xcodeTeams.first(where: { !$0.isFree })?.teamId ?? xcodeTeams.first?.teamId ?? "" }
        await refreshDevices()
    }

    func refreshDevices() async {
        let r = await devicesService.allDevices()
        devices = r.devices; deviceNote = r.note
        // the chosen destination wins over the remembered device id (they are stored separately)
        if case .device(let id) = destination, devices.contains(where: { $0.id == id }) { iosDeviceId = id }
        if iosDeviceId.isEmpty || !devices.contains(where: { $0.id == iosDeviceId }) {
            iosDeviceId = devices.first(where: { $0.connected && $0.supported && !$0.simulator })?.id ?? devices.first?.id ?? iosDeviceId
        }
    }

    func installToDevice(_ app: URL? = nil) async {
        guard let app = app ?? builtApp, let dev = selectedDevice else { installMessage = String(localized: "Choose a connected device first."); return }
        if !dev.supported { installMessage = String(localized: "\(dev.name) runs iOS \(dev.osVersion); Napple apps need iOS or iPadOS 26 or later."); return }
        if !dev.developerMode { installMessage = String(localized: "Turn on Developer Mode on \(dev.name): Settings > Privacy & Security > Developer Mode, then restart it."); return }
        installing = true; installMessage = String(localized: "Installing on \(dev.name)…")
        let id = NappleCLI.bundleId(of: app) ?? effectiveBundleId
        for l in DeviceService.commandLines(app: app, bundleId: id, on: dev) { appendLog("$ " + l) }
        var message: String
        do {
            try await devicesService.install(app: app, on: dev)
            if iosLaunchAfterInstall { try await devicesService.launch(bundleId: id, on: dev) }
            message = iosLaunchAfterInstall ? String(localized: "Installed and launched on \(dev.name).") : String(localized: "Installed on \(dev.name).")
        } catch { message = error.localizedDescription }
        await Task.yield()
        installMessage = message
        installing = false
    }

    /// Install a previously exported .ipa or built app on the selected device (Devices pane).
    func installFile() {
        let picked = pick(files: true, dirs: false, message: String(localized: "Choose a Napple .ipa or iOS .app"), types: [.application, .init(filenameExtension: "ipa") ?? .data])
        guard let f = picked.first else { return }
        Task {
            var app = f
            if f.pathExtension == "ipa" {
                do { app = try await devicesService.unpack(ipa: f, into: FileManager.default.temporaryDirectory.appendingPathComponent("napple-ipa-\(UUID().uuidString)")) }
                catch { installMessage = error.localizedDescription; return }
            }
            await installToDevice(app)
        }
    }
}
