import SwiftUI
import AppKit
import NappleStudioCore

/// What was read from the dropped files: choose updates and DLC, the destination and keys, check compatibility, build.
struct PlanView: View {
    @Environment(AppModel.self) private var m
    @State private var showSigning = false

    var body: some View {
        Group {
            if let plan = m.plan { content(plan) }
            else if m.phase == .failed { failure }
            else { reading }
        }
        .task { await m.refreshDevices() }
        .sheet(isPresented: $showSigning) { IOSSigningSheet() }
    }

    // MARK: states without a plan

    private var reading: some View {
        VStack(spacing: 14) {
            ProgressView().controlSize(.large)
            Text("Reading packages…").font(.system(size: 15, weight: .medium))
            Text("This can take a minute for large games.").font(.system(size: 12)).foregroundStyle(Palette.tertiary)
            Button("Cancel") { m.goBack() }.buttonStyle(SecondaryPill(height: 32))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var failure: some View {
        VStack(spacing: 14) {
            Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 34)).foregroundStyle(Palette.danger)
            Text("Couldn't read these files").font(.system(size: 20, weight: .semibold, design: .serif))
            Text(m.errorText ?? "Unknown error.").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                .multilineTextAlignment(.center).frame(maxWidth: 520).textSelection(.enabled)
            HStack(spacing: 10) {
                Button("Try again") { Task { await m.replan() } }.buttonStyle(PrimaryPill(height: 38))
                Button("Choose other files") { m.startOver() }.buttonStyle(SecondaryPill(height: 38, fontSize: 13))
                Button("Settings") { m.openSettings() }.buttonStyle(SecondaryPill(height: 38, fontSize: 13))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity).padding(40)
    }

    // MARK: the plan

    private func content(_ plan: PlanResult) -> some View {
        HStack(alignment: .top, spacing: 28) {
            VStack(alignment: .leading, spacing: 14) {
                titleRow(plan)
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        if let note = m.inputNote { Banner(text: note, tone: .warn) }
                        ForEach(Array(plan.warnings.prefix(3)), id: \.self) { Banner(text: $0, tone: .neutral) }
                        if plan.warnings.count > 3 { Text("and \(plan.warnings.count - 3) more notes in the log.").font(.system(size: 12)).foregroundStyle(Palette.tertiary) }
                        baseSection(plan)
                        if !plan.updates.isEmpty { listSection("Updates · pick at launch", plan.updates, isUpdate: true) }
                        if !plan.dlc.isEmpty { listSection("DLC · toggled at launch", plan.dlc, isUpdate: false) }
                    }
                    .padding(.bottom, 4)
                }
                .scrollIndicators(.automatic)
                HStack(spacing: 12) {
                    labeled("Build for") { DestinationMenu() }
                    labeled("Keys and firmware") { KeysMenu() }
                }
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)
            sidebar(plan)
        }
        .padding(.horizontal, 40).padding(.top, 12).padding(.bottom, 20)
        .disabled(m.phase == .building)
    }

    private func titleRow(_ plan: PlanResult) -> some View {
        HStack(spacing: 16) {
            GameTile(size: 72, radius: 18)
            VStack(alignment: .leading, spacing: 4) {
                Text(m.planTitle).font(.system(size: 28, weight: .medium, design: .serif)).lineLimit(2)
                    .minimumScaleFactor(0.7).accessibilityAddTraits(.isHeader)
                HStack(spacing: 8) {
                    Text(plan.base?.titleId ?? "No base game").font(.system(size: 13, design: .monospaced)).foregroundStyle(Palette.tertiary)
                    Text("· \(m.files.count) \(m.files.count == 1 ? "package" : "packages") read").font(.system(size: 13)).foregroundStyle(Palette.tertiary)
                    if m.phase == .planning { ProgressView().controlSize(.small) }
                }
            }
            Spacer(minLength: 0)
            Button("Add files…") { m.pickFilesAndAdd() }.buttonStyle(SecondaryPill(height: 30))
                .help("Add updates or DLC (or drop them on the window)")
                .accessibilityLabel("Add more files")
        }
    }

    @ViewBuilder private func baseSection(_ plan: PlanResult) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Base game")
            if let b = plan.base {
                HStack(spacing: 12) {
                    Image(systemName: "gamecontroller").font(.system(size: 15)).foregroundStyle(Palette.red)
                        .frame(width: 32, height: 32).background(Palette.fillStrong, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    Text("Version \(b.versionLabel)").font(.system(size: 14, weight: .medium)).lineLimit(1)
                    Spacer()
                    Text(PlanResult.formatBytes(b.size)).font(.system(size: 13)).foregroundStyle(Palette.tertiary).monospacedDigit()
                }
                .card(radius: 20, padding: 12)
                .accessibilityElement(children: .combine)
            } else {
                HStack(spacing: 10) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Palette.amber)
                    Text("No base game found. Add the base game too.").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                    Spacer()
                    Button("Add files…") { m.pickFilesAndAdd() }.buttonStyle(SecondaryPill(height: 28))
                }
                .card(radius: 20, padding: 12, tint: Palette.amber.opacity(0.12))
            }
        }
    }

    private func listSection(_ title: String, _ items: [PlanItem], isUpdate: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel(title)
            ForEach(items) { item in
                HStack(spacing: 12) {
                    Toggle(isOn: Binding(get: { item.selected }, set: { _ in m.toggle(updates: isUpdate, id: item.id) })) {
                        Text(isUpdate ? "Update \(item.versionLabel)" : item.titleId)
                            .font(.system(size: 14, weight: .medium, design: isUpdate ? .default : .monospaced)).lineLimit(1)
                    }
                    .toggleStyle(.checkbox)
                    .accessibilityLabel("Include \(isUpdate ? "update" : "DLC") \(isUpdate ? item.versionLabel : item.titleId), adds \(PlanResult.formatBytes(item.size))")
                    Spacer()
                    Text("+" + PlanResult.formatBytes(item.size)).font(.system(size: 13)).foregroundStyle(Palette.tertiary).monospacedDigit()
                }
                .card(radius: 20, padding: 12)
            }
        }
    }

    private func labeled<V: View>(_ title: String, @ViewBuilder _ field: () -> V) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 12, weight: .semibold)).foregroundStyle(Palette.tertiary)
            field()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: right column

    private func sidebar(_ plan: PlanResult) -> some View {
        VStack(spacing: 12) {
            CompatCard(entry: m.compat, hasBase: plan.base != nil)
            VStack(alignment: .leading, spacing: 6) {
                if let e = m.errorText { Banner(text: e, tone: .bad) }
                if let w = m.destinationBlocker { Banner(text: w, tone: .warn) }
                else if let d = m.diskWarning, m.errorText == nil { Banner(text: d, tone: .bad) }
                if m.destination.isIOS {
                    Button { showSigning = true } label: { Label("Signing…", systemImage: "key.horizontal") }
                        .buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Signing options for iPhone and iPad")
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Divider().overlay(Palette.stroke)
            HStack(alignment: .firstTextBaseline) {
                Text("Estimated app size").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                Spacer()
                Text(PlanResult.formatBytes(plan.totalSelectedBytes)).font(.system(size: 20, weight: .semibold)).monospacedDigit()
            }
            .accessibilityElement(children: .combine)
            Button { m.startBuild() } label: { Text("Build app").frame(maxWidth: .infinity) }
                .buttonStyle(PrimaryPill(height: 48, fontSize: 16))
                .disabled(!m.canBuild)
                .keyboardShortcut(.defaultAction)
                .accessibilityLabel("Build the app")
        }
        .frame(width: 340)
    }
}

// MARK: compatibility card

/// Pulled from compat/matrix.json: overall result, how many stages pass, what still fails, and every stage.
struct CompatCard: View {
    let entry: CompatEntry?
    let hasBase: Bool

    var body: some View {
        Group {
            if let e = entry { known(e) } else { unknown }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .card(radius: 14, padding: 14, tint: tint)
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(border))
    }

    private var tint: Color {
        switch entry?.status.tone ?? .neutral {
        case .good: return Palette.mint.opacity(0.12)
        case .warn: return Palette.amber.opacity(0.12)
        case .bad: return Palette.danger.opacity(0.12)
        case .neutral: return Palette.fill
        }
    }
    private var border: Color { (entry?.status.tone ?? .neutral) == .neutral ? .clear : (entry?.status.tone.color ?? .clear).opacity(0.35) }

    private var unknown: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(hasBase ? "Not in the compatibility matrix yet" : "Compatibility unknown", systemImage: "questionmark.circle")
                .font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.secondary)
            Text(hasBase ? "Nobody has tested this game with Napple, so it may not run well. You can still build it." : "Add the base game to see how well it runs.")
                .font(.system(size: 12)).foregroundStyle(Palette.tertiary).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
    }

    private func known(_ e: CompatEntry) -> some View {
        let total = e.stages.count
        return VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label(e.status.label, systemImage: "shield").font(.system(size: 15, weight: .semibold)).foregroundStyle(e.status.tone.color)
                Spacer()
                if total > 0 { Text("\(e.passCount) of \(total) stages pass").font(.system(size: 12)).foregroundStyle(Palette.secondary) }
            }
            if total > 0 {
                HStack(spacing: 3) {
                    ForEach(Array(e.stages.enumerated()), id: \.offset) { _, s in
                        Capsule().fill(stageTone(s.status).color).frame(height: 6)
                    }
                }
                .accessibilityHidden(true)
            }
            if !e.status.isAtLeastPartial {
                Text("This game may not run well yet.").font(.system(size: 12, weight: .medium)).foregroundStyle(Palette.amber)
            }
            if let n = e.attentionNote ?? e.notes {
                Text(n).font(.system(size: 12)).foregroundStyle(Palette.secondary).lineLimit(4).fixedSize(horizontal: false, vertical: true)
            }
            if total > 0 {
                Divider().overlay(Palette.stroke)
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 12, alignment: .leading), GridItem(.flexible(), alignment: .leading)], spacing: 0) {
                    ForEach(e.stages, id: \.name) { s in
                        HStack(spacing: 8) {
                            Image(systemName: stageSymbol(s.status)).font(.system(size: 12, weight: .semibold))
                                .foregroundStyle(stageTone(s.status).color).frame(width: 14)
                            Text(CompatEntry.stageTitle(s.name)).font(.system(size: 12)).foregroundStyle(Palette.secondary).lineLimit(1)
                        }
                        .padding(.vertical, 5)
                        .help(s.reason ?? s.status.capitalized)
                        .accessibilityElement(children: .combine)
                        .accessibilityLabel("\(CompatEntry.stageTitle(s.name)): \(s.status)")
                    }
                }
            }
            Spacer(minLength: 0)
        }
    }
}

// MARK: pickers

/// "This Mac", connected iOS 26+ devices, or an .ipa file for iPhone or iPad.
struct DestinationMenu: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Menu {
            Button { m.destination = .mac } label: { Label("This Mac", systemImage: m.destination == .mac ? "checkmark" : "desktopcomputer") }
            Divider()
            Section("Connected devices (iOS 26 or later)") {
                if m.devices.isEmpty { Text("No devices found") }
                ForEach(m.devices) { d in
                    Button { m.destination = .device(d.id) } label: {
                        Label(d.summary, systemImage: m.destination == .device(d.id) ? "checkmark" : (d.name.localizedCaseInsensitiveContains("ipad") ? "ipad" : "iphone"))
                    }
                    .disabled(!d.connected || !d.supported)
                }
            }
            Section("Save as file") {
                Button { m.destination = .ipaIPhone } label: { Label("iOS .ipa file", systemImage: m.destination == .ipaIPhone ? "checkmark" : "shippingbox") }
                Button { m.destination = .ipaIPad } label: { Label("iPadOS .ipa file", systemImage: m.destination == .ipaIPad ? "checkmark" : "shippingbox") }
            }
            Divider()
            Button("Rescan devices") { Task { await m.refreshIOS() } }
        } label: {
            HStack {
                Text(m.destinationLabel).lineLimit(1)
                Spacer(minLength: 4)
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold)).foregroundStyle(Palette.tertiary)
            }
            .font(.system(size: 14)).padding(.horizontal, 12).frame(height: 40)
            .background(Palette.fill, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Palette.strokeStrong))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden)
        .accessibilityLabel("Build destination: \(m.destinationLabel)")
    }
}

struct KeysMenu: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Menu {
            ForEach(m.sets) { s in
                Button { m.selectedSetName = s.name } label: { Label(s.name, systemImage: s.name == m.selectedSet?.name ? "checkmark" : "key") }
            }
            if m.sets.isEmpty { Text("No key sets") }
            Divider()
            Button("Manage in Settings…") { m.openSettings() }
        } label: {
            HStack {
                Text(m.selectedSet?.name ?? "Choose keys").lineLimit(1)
                Spacer(minLength: 4)
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold)).foregroundStyle(Palette.tertiary)
            }
            .font(.system(size: 14)).padding(.horizontal, 12).frame(height: 40)
            .background(Palette.fill, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Palette.strokeStrong))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden)
        .accessibilityLabel("Key and firmware set: \(m.selectedSet?.name ?? "none")")
    }
}

/// iOS / iPadOS 26+ signing choices (everything local: your Apple ID certificate, a provisioning profile Xcode created, your device).
struct IOSSigningSheet: View {
    @Environment(AppModel.self) private var m
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var m = m
        VStack(alignment: .leading, spacing: 12) {
            Text("Signing for iPhone and iPad").font(.system(size: 18, weight: .semibold, design: .serif))
            Toggle("Unsigned (sign it yourself with AltStore, Sideloadly, …)", isOn: $m.iosUnsigned).toggleStyle(.checkbox)
            if !m.iosUnsigned {
                Picker("Identity", selection: $m.iosIdentity) {
                    if m.signing?.identities.isEmpty ?? true { Text("None found").tag("") }
                    ForEach(m.signing?.identities ?? []) { Text($0.name).tag($0.sha1) }
                }.pickerStyle(.menu).accessibilityLabel("Code signing identity")
                Picker("Profile", selection: $m.iosProfilePath) {
                    if m.signing?.profiles.isEmpty ?? true { Text("None found").tag("") }
                    ForEach(m.signing?.profiles ?? []) { Text("\($0.name) (\($0.appId))").tag($0.path) }
                }.pickerStyle(.menu).accessibilityLabel("Provisioning profile")
                TextField(m.selectedProfile?.explicitBundleId ?? "Bundle id (default com.napple.game.<title id>)", text: $m.iosBundleId)
                    .textFieldStyle(.roundedBorder).accessibilityLabel("Bundle identifier")
                Toggle("Also export an .ipa", isOn: $m.iosExportIPA).toggleStyle(.checkbox)
                Toggle("Install after build", isOn: $m.iosInstallAfterBuild).toggleStyle(.checkbox)
                Toggle("Launch after install", isOn: $m.iosLaunchAfterInstall).toggleStyle(.checkbox).disabled(!m.iosInstallAfterBuild)
            }
            HStack {
                Button { Task { await m.refreshIOS() } } label: { Label("Rescan", systemImage: "arrow.clockwise") }
                    .buttonStyle(SecondaryPill(height: 28)).help("Rescan identities, profiles and devices")
                Spacer()
                Button("Done") { dismiss() }.buttonStyle(PrimaryPill(height: 36)).keyboardShortcut(.defaultAction)
            }
            if let why = m.destinationBlocker { Banner(text: why, tone: .warn) }
            Text("Needs iOS or iPadOS 26+, Developer Mode on the device, and the game's memory fitting the device. A free Apple ID profile expires after 7 days.")
                .font(.system(size: 12)).foregroundStyle(Palette.tertiary)
        }
        .padding(24).frame(width: 520)
        .task { if m.signing == nil { await m.refreshIOS() } }
    }
}
