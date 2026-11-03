import SwiftUI
import NappleStudioCore

/// iPhone & iPad: devices, signing, bundle id, memory limit, installing an existing file.
struct DevicesSettingsTab: View {
    @Environment(AppModel.self) private var m
    @State private var refreshing = false
    @State private var showCommands = false

    var body: some View {
        @Bindable var m = m
        SettingsPage {
            devicesSection
            signingSection
            bundleSection
            memorySection
            installSection
            commandsSection
        }
        .task { if m.signing == nil { await refresh() } }
    }

    private func refresh() async {
        refreshing = true
        await m.refreshIOS()
        refreshing = false
    }

    // MARK: devices

    private var devicesSection: some View {
        Section {
            if m.devices.isEmpty {
                ContentUnavailableView {
                    Label("No device found", systemImage: "iphone.slash")
                } description: {
                    Text("Plug in an iPhone or iPad running iOS 26 or later, unlock it and trust this Mac. Turn on Developer Mode in Settings > Privacy & Security. Booted iOS Simulators show up here too.")
                }
                .frame(maxWidth: .infinity)
                .accessibilityIdentifier("settings.devices.empty")
            } else {
                ForEach(m.devices) { d in deviceRow(d) }
            }
            if let n = m.deviceNote { SettingsBanner(text: n, tone: .warn) }
            HStack {
                Button { Task { await refresh() } } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    .buttonStyle(.glass).disabled(refreshing).accessibilityIdentifier("settings.devices.refresh")
                if refreshing { ProgressView().controlSize(.small) }
            }
        } header: {
            SettingsHeader("Devices")
        } footer: {
            SettingsFootnote("Choosing a device is all it takes to install on it: Studio installs there after building.")
        }
    }

    private func deviceRow(_ d: IOSDevice) -> some View {
        let selected = m.iosDeviceId == d.id
        let (state, color) = deviceState(d)
        return Button {
            m.iosDeviceId = d.id
            m.destination = .device(d.id)
        } label: {
            HStack(spacing: 10) {
                Image(systemName: d.simulator ? "iphone.and.arrow.right.inward" : "iphone.gen3").font(.title3).foregroundStyle(.tint).frame(width: 26)
                VStack(alignment: .leading, spacing: 1) {
                    Text(d.name).fontWeight(.medium).lineLimit(1)
                    Text("\(d.model) · \(d.simulator ? String(localized: "Simulator") : "iOS") \(d.osVersion)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 8)
                Label { Text(state) } icon: { Image(systemName: "circle.fill").font(.system(size: 7)) }.font(.caption).foregroundStyle(color)
                Image(systemName: "checkmark").foregroundStyle(.tint).opacity(selected ? 1 : 0).frame(width: 14).accessibilityHidden(true)
            }
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
        .accessibilityIdentifier("settings.devices.row.\(d.id)")
    }

    private func deviceState(_ d: IOSDevice) -> (LocalizedStringKey, Color) {
        if d.simulator { return d.supported ? ("Booted (simulator)", .green) : ("Needs iOS 26", .orange) }
        if !d.connected { return ("Not connected", .secondary) }
        if !d.developerMode { return ("Developer Mode off", .orange) }
        if !d.supported { return ("Needs iOS 26", .orange) }
        return ("Ready", .green)
    }

    // MARK: signing

    private var signingSection: some View {
        @Bindable var m = m
        return Section {
            VStack(alignment: .leading, spacing: 12) {
                SettingsRadioRow(title: "Automatic (Xcode)", detail: "Xcode creates and renews the profile for each game. Needs an Apple ID signed in to Xcode.",
                                 selected: m.signingMode == .automatic, identifier: "settings.devices.mode.automatic") { m.signingMode = .automatic }
                SettingsRadioRow(title: "Provisioning profile", detail: "Sign with a profile and certificate you already have on this Mac.",
                                 selected: m.signingMode == .profile, identifier: "settings.devices.mode.profile") { m.signingMode = .profile }
                SettingsRadioRow(title: "Unsigned", detail: "Leave the app unsigned for AltStore, Sideloadly or your own signing tools. Nothing is installed from Studio.",
                                 selected: m.signingMode == .unsigned, identifier: "settings.devices.mode.unsigned") { m.signingMode = .unsigned }
            }
            .padding(.vertical, 2)
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Signing mode")

            switch m.signingMode {
            case .automatic: automaticControls
            case .profile: profileControls
            case .unsigned: SettingsBanner(text: String(localized: "The app and .ipa are left unsigned. Sign them with AltStore, Sideloadly or your own tools, then install them there."), tone: .info)
            }
            if m.signingMode != .unsigned {
                LabeledContent("Account") {
                    Label(m.accountKind.label, systemImage: m.accountKind == .paid ? "person.badge.shield.checkmark" : "person")
                        .foregroundStyle(m.accountKind == .paid ? Color.green : (m.accountKind == .free ? Color.orange : Color.secondary))
                        .accessibilityIdentifier("settings.devices.account")
                }
            }
            if let b = m.destinationBlocker, m.destination.isIOS { SettingsBanner(text: b, tone: .warn) }
        } header: { SettingsHeader("Signing mode") }
    }

    @ViewBuilder private var automaticControls: some View {
        @Bindable var m = m
        if m.xcodeTeams.isEmpty {
            SettingsBanner(text: String(localized: "Xcode has no signed-in account. Open Xcode > Settings > Accounts, add your Apple ID, then press Refresh."), tone: .warn)
                .accessibilityIdentifier("settings.devices.noAccount")
        } else {
            Picker("Team", selection: $m.teamId) {
                ForEach(m.xcodeTeams, id: \.teamId) { t in Text("\(t.name) · \(t.teamId) · \(t.isFree ? String(localized: "free") : String(localized: "paid"))").tag(t.teamId) }
            }
            .accessibilityIdentifier("settings.devices.team")
        }
    }

    @ViewBuilder private var profileControls: some View {
        @Bindable var m = m
        let identities = m.signing?.identities ?? [], profiles = m.signing?.profiles ?? []
        if identities.isEmpty {
            SettingsBanner(text: String(localized: "No code-signing identity found. In Xcode: Settings > Accounts > Manage Certificates > + Apple Development."), tone: .warn)
        } else {
            Picker("Identity", selection: $m.iosIdentity) { ForEach(identities) { Text($0.name).tag($0.sha1) } }.accessibilityIdentifier("settings.devices.identity")
        }
        if profiles.isEmpty {
            SettingsBanner(text: String(localized: "No provisioning profiles found in ~/Library/Developer/Xcode/UserData/Provisioning Profiles or ~/Library/MobileDevice/Provisioning Profiles."), tone: .warn)
        } else {
            Picker("Profile", selection: $m.iosProfilePath) { ForEach(profiles) { Text("\($0.name) · \($0.appId)").tag($0.path) } }.accessibilityIdentifier("settings.devices.profile")
            if let d = m.selectedProfileDetails {
                LabeledContent("Team") { Text("\(d.teamName) (\(d.teamId))") }
                LabeledContent("Devices") { Text(d.provisionsAllDevices ? String(localized: "All devices") : "\(d.deviceCount)") }
                LabeledContent("Expires") { Text(d.expires.map { $0.formatted(date: .abbreviated, time: .omitted) } ?? "—").foregroundStyle((d.expires ?? .distantFuture) < .now ? .red : .primary) }
                LabeledContent("Memory limit") { Text(d.grantsIncreasedMemory ? "Granted" : "Not granted").foregroundStyle(d.grantsIncreasedMemory ? Color.green : Color.secondary) }
            } else if m.selectedProfile != nil {
                SettingsFootnote("This profile could not be read.", tone: .orange)
            }
        }
    }

    // MARK: bundle id

    private var bundleSection: some View {
        @Bindable var m = m
        return Section {
            LabeledContent("Prefix") {
                TextField(BundleIDs.defaultPrefix, text: $m.iosBundlePrefix).labelsHidden().textFieldStyle(.roundedBorder).monospaced().frame(maxWidth: 260)
                    .accessibilityIdentifier("settings.devices.bundlePrefix")
            }
            LabeledContent("Example") { Text(m.effectiveBundleId).monospaced().foregroundStyle(.secondary).textSelection(.enabled).lineLimit(1).truncationMode(.middle) }
            if let n = m.bundleIdNote { SettingsBanner(text: n, tone: .info) }
        } header: { SettingsHeader("Bundle id prefix") } footer: {
            SettingsFootnote("Each game gets its own id (prefix plus the title id), so several games can be installed side by side.")
        }
    }

    // MARK: memory

    private var memorySection: some View {
        let plan = m.entitlementPlan
        let free = m.accountKind == .free
        return Section {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: plan.includesMemory ? "checkmark.shield.fill" : "shield").font(.title3).frame(width: 24)
                    .foregroundStyle(free ? Color.orange : (plan.includesMemory ? Color.green : Color.secondary))
                VStack(alignment: .leading, spacing: 4) {
                    Text(plan.includesMemory ? "Increased Memory Limit will be requested" : "Normal memory limit").fontWeight(.medium)
                    Text(plan.explanation).font(.callout).foregroundStyle(free ? Color.orange : Color.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("settings.devices.memory")
        } header: { SettingsHeader("Memory limit") }
    }

    // MARK: install

    private var installSection: some View {
        @Bindable var m = m
        return Section {
            LabeledContent("Install an existing file") {
                Button("Install .ipa or .app…") { m.installFile() }
                    .buttonStyle(.glass).disabled(m.selectedDevice == nil || m.installing).accessibilityIdentifier("settings.devices.install")
            }
            if m.selectedDevice == nil { SettingsFootnote("Choose a device above first.") }
            if let msg = m.installMessage {
                SettingsBanner(text: msg, tone: m.installing ? .info : (m.installFailed ? .bad : .good), busy: m.installing).accessibilityIdentifier("settings.devices.installMessage")
            }
            Toggle("Launch after install", isOn: $m.iosLaunchAfterInstall).accessibilityIdentifier("settings.devices.launchAfterInstall")
        } header: { SettingsHeader("Install") }
    }

    // MARK: commands

    private var commandsSection: some View {
        Section {
            DisclosureGroup("Command lines Studio runs", isExpanded: $showCommands) {
                VStack(alignment: .leading, spacing: 4) {
                    if let d = m.selectedDevice {
                        ForEach(DeviceService.commandLines(app: m.builtApp ?? URL(fileURLWithPath: "<Game>.app"), bundleId: m.effectiveBundleId, on: d), id: \.self) { Text($0) }
                    }
                    Text("xcrun devicectl list devices --json-output <file>")
                    Text("xcrun simctl list devices booted -j")
                    if m.signingMode == .automatic { Text(Provisioner.commandLine(projectDir: URL(fileURLWithPath: "<work>")).joined(separator: " ")) }
                }
                .font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(.top, 6)
            }
            .accessibilityIdentifier("settings.devices.commands")
        }
    }
}
