import SwiftUI
import AppKit
import NappleStudioCore

/// iPhone & iPad: paired devices, the guided signing flow, memory-limit explanation, install.
struct DevicesView: View {
    @Environment(AppModel.self) private var m
    @State private var showCommands = false
    var body: some View {
        @Bindable var m = m
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                GlassCard(title: "Devices", symbol: "iphone.gen3") {
                    if m.devices.isEmpty {
                        ContentUnavailableView {
                            Label("No device found", systemImage: "iphone.slash")
                        } description: {
                            Text(m.deviceNote ?? String(localized: "Plug in an iPhone or iPad with iOS 26 or later, unlock it and trust this Mac. Booted iOS Simulators show up here too."))
                        } actions: {
                            Button("Refresh") { Task { await m.refreshIOS() } }
                        }
                        .frame(maxWidth: .infinity)
                    } else {
                        ForEach(m.devices) { d in
                            HStack(spacing: 10) {
                                Image(systemName: d.simulator ? "iphone.and.arrow.right.inward" : "iphone.gen3").font(.title3).foregroundStyle(.tint).frame(width: 24)
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(d.name).font(.subheadline.weight(.semibold)).lineLimit(1).truncationMode(.tail)
                                    Text("\(d.model) · \(d.simulator ? "Simulator" : "iOS") \(d.osVersion)").font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
                                }
                                .layoutPriority(1)
                                Spacer(minLength: 6)
                                deviceState(d).fixedSize()
                                Image(systemName: "checkmark.circle.fill").foregroundStyle(.tint).opacity(m.iosDeviceId == d.id ? 1 : 0).frame(width: 16).accessibilityLabel("Selected")
                            }
                            .padding(6)
                            .contentShape(.rect)
                            .onTapGesture { m.iosDeviceId = d.id; m.destination = .device(d.id) }
                            .accessibilityElement(children: .combine)
                            .accessibilityAddTraits(.isButton)
                            .accessibilityIdentifier("devices.row.\(d.id)")
                        }
                        if let n = m.deviceNote { Banner(text: n, tone: .warn) }
                        ViewThatFits(in: .horizontal) {
                            HStack {
                                Button("Refresh") { Task { await m.refreshIOS() } }
                                Button("Install .ipa or .app…") { m.installFile() }.disabled(m.selectedDevice == nil)
                                Spacer()
                                Toggle("Launch after install", isOn: $m.iosLaunchAfterInstall).toggleStyle(.switch).controlSize(.small)
                            }
                            VStack(alignment: .leading, spacing: 6) {
                                HStack { Button("Refresh") { Task { await m.refreshIOS() } }; Button("Install .ipa or .app…") { m.installFile() }.disabled(m.selectedDevice == nil) }
                                Toggle("Launch after install", isOn: $m.iosLaunchAfterInstall).toggleStyle(.switch).controlSize(.small)
                            }
                        }
                        if let msg = m.installMessage, m.pane == .devices { Banner(text: msg, tone: m.installing ? .info : .good) }
                    }
                }

                SigningCard()

                GlassCard(title: "Memory limit", symbol: "memorychip") {
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: m.entitlementPlan.includesMemory ? "checkmark.shield.fill" : "shield").foregroundStyle(m.entitlementPlan.includesMemory ? .green : .secondary).font(.title3).frame(width: 24)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(m.entitlementPlan.includesMemory ? "Increased Memory Limit will be requested" : "Normal memory limit").font(.subheadline.weight(.semibold))
                            Text(m.entitlementPlan.explanation).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                            if !m.entitlementPlan.entitlements.isEmpty {
                                ForEach(m.entitlementPlan.entitlements.keys.sorted(), id: \.self) { Text($0).font(.caption.monospaced()).foregroundStyle(.tertiary).lineLimit(1).truncationMode(.middle) }
                            }
                        }
                    }
                    .accessibilityIdentifier("devices.memory")
                }

                DisclosureGroup("Command lines Studio runs", isExpanded: $showCommands) {
                    VStack(alignment: .leading, spacing: 4) {
                        if let d = m.selectedDevice {
                            ForEach(DeviceService.commandLines(app: m.builtApp ?? URL(fileURLWithPath: "<Game>.app"), bundleId: m.effectiveBundleId, on: d), id: \.self) { Text($0) }
                        }
                        Text("xcrun devicectl list devices --json-output <file>")
                        Text("xcrun simctl list devices booted -j")
                        if m.signingMode == .automatic { Text(Provisioner.commandLine(projectDir: URL(fileURLWithPath: "<work>")).joined(separator: " ")) }
                    }
                    .font(.caption.monospaced()).textSelection(.enabled).padding(.top, 6)
                }
                .padding(.horizontal, 4)
            }
            .padding(14)
        }
        .task { if m.signing == nil { await m.refreshIOS() } }
    }

    @ViewBuilder private func deviceState(_ d: IOSDevice) -> some View {
        let (text, color): (LocalizedStringKey, Color) = !d.connected ? ("Not connected", .secondary) : !d.developerMode ? ("Developer Mode off", .orange) : !d.supported ? ("Needs iOS 26", .orange) : ("Ready", .green)
        Label(text, systemImage: "circle.fill").font(.caption).foregroundStyle(color).labelStyle(.titleAndIcon)
    }
}

/// Guided signing: mode, team or identity + profile, bundle id, export method, debug build.
struct SigningCard: View {
    @Environment(AppModel.self) private var m
    /// A label above its control: stays readable and never overflows when the window is narrow.
    private func field<C: View>(_ label: LocalizedStringKey, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 3) { Text(label).font(.caption).foregroundStyle(.secondary); content() }
    }
    var body: some View {
        @Bindable var m = m
        GlassCard(title: "Signing", symbol: "signature") {
            Picker("Mode", selection: $m.signingMode) { ForEach(SigningMode.allCases) { Text($0.shortLabel).tag($0).help($0.label) } }.pickerStyle(.segmented).labelsHidden()
            switch m.signingMode {
            case .automatic:
                if m.xcodeTeams.isEmpty {
                    Banner(text: String(localized: "Xcode has no signed-in account. Open Xcode > Settings > Accounts and add your Apple ID, then press Refresh."), tone: .warn)
                } else {
                    field("Team") {
                        Picker("Team", selection: $m.teamId) {
                            ForEach(m.xcodeTeams, id: \.teamId) { t in Text("\(t.name) · \(t.teamId) · \(t.isFree ? "free" : "paid")").tag(t.teamId) }
                        }.labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                    }
                    Text("Before each build Xcode creates or refreshes the development profile for the game's bundle id (registering the connected device and turning on the App ID capabilities); napple then signs the app and writes the .ipa with it.").font(.caption).foregroundStyle(.secondary)
                }
            case .profile:
                field("Identity") {
                    Picker("Identity", selection: $m.iosIdentity) {
                        ForEach(m.signing?.identities ?? []) { Text($0.name).tag($0.sha1) }
                    }.labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                }
                field("Profile") {
                    Picker("Profile", selection: $m.iosProfilePath) {
                        ForEach(m.signing?.profiles ?? []) { p in Text("\(p.name) · \(p.appId) · expires \(p.expires)").tag(p.path) }
                    }.labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                }
                if let d = m.selectedProfileDetails {
                    Text("\(d.teamName) (\(d.teamId)) · \(d.deviceCount) devices · \(d.grantsIncreasedMemory ? "grants" : "no") increased memory").font(.caption).foregroundStyle(.secondary)
                }
            case .unsigned:
                Text("The app and .ipa are left unsigned for AltStore, Sideloadly or your own signing tools. Nothing can be installed directly from Studio.").font(.caption).foregroundStyle(.secondary)
            }
            field("Bundle ID prefix") {
                TextField(BundleIDs.defaultPrefix, text: $m.iosBundlePrefix).labelsHidden().frame(maxWidth: .infinity).monospaced()
            }
            Text("Each game gets its own id, \(m.effectiveBundleId) for the current one, so several games can be installed side by side.").font(.caption).foregroundStyle(.secondary)
            if let n = m.bundleIdNote { Banner(text: n, tone: .info) }
            Toggle("Debug build", isOn: $m.iosDebugBuild).toggleStyle(.switch).controlSize(.small).help("Adds the debuggable memory entitlement on paid accounts")
            HStack {
                Label(m.accountKind.label, systemImage: m.accountKind == .paid ? "person.badge.shield.checkmark" : "person").foregroundStyle(m.accountKind == .paid ? .green : .secondary)
                    .accessibilityIdentifier("signing.account")
                Spacer()
                Button("Refresh") { Task { await m.refreshIOS() } }
            }
            if let b = m.destinationBlocker, m.destination.isIOS { Banner(text: b, tone: .warn) }
        }
    }
}
