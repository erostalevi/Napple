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
            VStack(alignment: .leading, spacing: 16) {
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
                            HStack(spacing: 12) {
                                Image(systemName: d.simulator ? "iphone.and.arrow.right.inward" : "iphone.gen3").font(.title2).foregroundStyle(.tint)
                                VStack(alignment: .leading) {
                                    Text(d.name).font(.headline)
                                    Text("\(d.model) · \(d.simulator ? "Simulator" : "iOS") \(d.osVersion)").font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                deviceState(d)
                                if m.iosDeviceId == d.id { Image(systemName: "checkmark.circle.fill").foregroundStyle(.tint).accessibilityLabel("Selected") }
                            }
                            .padding(8)
                            .contentShape(.rect)
                            .onTapGesture { m.iosDeviceId = d.id; m.destination = .device(d.id) }
                            .accessibilityElement(children: .combine)
                            .accessibilityAddTraits(.isButton)
                            .accessibilityIdentifier("devices.row.\(d.id)")
                        }
                        if let n = m.deviceNote { Banner(text: n, tone: .warn) }
                        HStack {
                            Button("Refresh") { Task { await m.refreshIOS() } }
                            Button("Install .ipa or .app…") { m.installFile() }.disabled(m.selectedDevice == nil)
                            Spacer()
                            Toggle("Launch after install", isOn: $m.iosLaunchAfterInstall).toggleStyle(.switch).controlSize(.small)
                        }
                        if let msg = m.installMessage, m.pane == .devices { Banner(text: msg, tone: m.installing ? .info : .good) }
                    }
                }

                SigningCard()

                GlassCard(title: "Memory limit", symbol: "memorychip") {
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: m.entitlementPlan.includesMemory ? "checkmark.shield.fill" : "shield").foregroundStyle(m.entitlementPlan.includesMemory ? .green : .secondary).font(.title2)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(m.entitlementPlan.includesMemory ? "Increased Memory Limit will be requested" : "Normal memory limit").font(.headline)
                            Text(m.entitlementPlan.explanation).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                            if !m.entitlementPlan.entitlements.isEmpty {
                                ForEach(m.entitlementPlan.entitlements.keys.sorted(), id: \.self) { Text($0).font(.caption.monospaced()).foregroundStyle(.tertiary) }
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
            .padding(24)
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
    var body: some View {
        @Bindable var m = m
        GlassCard(title: "Signing", symbol: "signature") {
            Picker("Mode", selection: $m.signingMode) { ForEach(SigningMode.allCases) { Text($0.label).tag($0) } }.pickerStyle(.segmented).labelsHidden()
            switch m.signingMode {
            case .automatic:
                if m.xcodeTeams.isEmpty {
                    Banner(text: String(localized: "Xcode has no signed-in account. Open Xcode > Settings > Accounts and add your Apple ID, then press Refresh."), tone: .warn)
                } else {
                    LabeledContent("Team") {
                        Picker("Team", selection: $m.teamId) {
                            ForEach(m.xcodeTeams, id: \.teamId) { t in Text("\(t.name) · \(t.teamId) · \(t.isFree ? "free" : "paid")").tag(t.teamId) }
                        }.labelsHidden().frame(maxWidth: 420)
                    }
                    Text("Before each build Xcode creates or refreshes the development profile for the game's bundle id (registering the connected device and turning on the App ID capabilities); napple then signs the app and writes the .ipa with it.").font(.caption).foregroundStyle(.secondary)
                }
            case .profile:
                LabeledContent("Identity") {
                    Picker("Identity", selection: $m.iosIdentity) {
                        ForEach(m.signing?.identities ?? []) { Text($0.name).tag($0.sha1) }
                    }.labelsHidden().frame(maxWidth: 420)
                }
                LabeledContent("Profile") {
                    Picker("Profile", selection: $m.iosProfilePath) {
                        ForEach(m.signing?.profiles ?? []) { p in Text("\(p.name) · \(p.appId) · expires \(p.expires)").tag(p.path) }
                    }.labelsHidden().frame(maxWidth: 420)
                }
                if let d = m.selectedProfileDetails {
                    Text("\(d.teamName) (\(d.teamId)) · \(d.deviceCount) devices · \(d.grantsIncreasedMemory ? "grants" : "no") increased memory").font(.caption).foregroundStyle(.secondary)
                }
            case .unsigned:
                Text("The app and .ipa are left unsigned for AltStore, Sideloadly or your own signing tools. Nothing can be installed directly from Studio.").font(.caption).foregroundStyle(.secondary)
            }
            LabeledContent("Bundle ID prefix") {
                TextField(BundleIDs.defaultPrefix, text: $m.iosBundlePrefix).labelsHidden().frame(maxWidth: 420).monospaced()
            }
            Text("Each game gets its own id, \(m.effectiveBundleId) for the current one, so several games can be installed side by side.").font(.caption).foregroundStyle(.secondary)
            if let n = m.bundleIdNote { Banner(text: n, tone: .info) }
            Toggle("Debug build (adds the debuggable memory entitlement on paid accounts)", isOn: $m.iosDebugBuild).toggleStyle(.switch).controlSize(.small)
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
