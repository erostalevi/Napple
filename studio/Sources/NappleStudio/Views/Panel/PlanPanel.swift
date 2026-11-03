import SwiftUI
import AppKit
import NappleStudioCore

/// The files are read: what goes in, and where it goes.
struct PlanPanel: View {
    @Environment(AppModel.self) private var m
    @Environment(\.openSettings) private var openSettings
    @AppStorage("playAfterBuild") private var playAfter = false

    private var blocked: Bool { m.plan?.base == nil || m.diskWarning != nil || m.destinationBlocker != nil }

    var body: some View {
        @Bindable var m = m
        PanelFrame {
            PanelHeader(title: m.planTitle, subtitle: subtitle) {
                if let c = m.compat { CompatBadge(entry: c) }
            }
        } content: {
            notices
            include
            VStack(alignment: .leading, spacing: 8) {
                SectionLabel("Build for")
                HStack(spacing: 6) {
                    choice("This Mac", ios: false, id: "plan.dest.mac")
                    choice("iOS Device", ios: true, id: "plan.dest.ios")
                }
                .padding(3).background(Theme.surface, in: .rect(cornerRadius: 11, style: .continuous))
                .accessibilityElement(children: .contain).accessibilityLabel(Text("Build for"))
                if m.destination.isIOS { iosOptions } else { macOptions }
            }
        } footer: {
            Button("Cancel") { m.closePanel() }.controlSize(.large).keyboardShortcut(.cancelAction).accessibilityIdentifier("plan.cancel")
            Spacer()
            Button { m.startBuild() } label: { Text(verbatim: m.buildButtonTitle).lineLimit(1) }
                .buttonStyle(.glassProminent).controlSize(.large)
                .disabled(!m.canBuild).keyboardShortcut(.defaultAction).accessibilityIdentifier("plan.build")
        }
        .task(id: m.destination.isIOS) { if m.destination.isIOS { await m.refreshDevices() } }
    }

    /// One half of the Mac / iOS switch.
    private func choice(_ title: LocalizedStringKey, ios: Bool, id: String) -> some View {
        let on = m.destination.isIOS == ios
        return Button { m.chooseDestination(ios: ios) } label: {
            Text(title).fontWeight(on ? .semibold : .regular).frame(maxWidth: .infinity).frame(height: 28)
                .background(on ? Theme.surfaceStrong : .clear, in: .rect(cornerRadius: 8, style: .continuous))
                .contentShape(.rect)
        }
        .buttonStyle(.plain).accessibilityAddTraits(on ? .isSelected : []).accessibilityIdentifier(id)
    }

    private var subtitle: String? {
        guard let b = m.plan?.base else { return nil }
        return "\(b.titleId) · \(b.versionLabel) · \(PlanResult.formatBytes(b.size))"
    }

    @ViewBuilder private var notices: some View {
        if m.cli == nil { Notice(text: String(localized: "The Napple engine was not found, so nothing can be built. Open Settings > Engine to pick or install one."), tone: .bad) }
        if m.plan?.base == nil { Notice(text: String(localized: "No base game in these files. Drop the base game (NSP or XCI) too."), tone: .warn) }
        if let d = m.diskWarning { Notice(text: d, tone: .bad) }
        if let b = m.destinationBlocker { Notice(text: b, tone: .bad) }
        if let c = m.compat, let note = c.attentionNote { Notice(text: note, tone: .warn) }
        ForEach(m.plan?.warnings ?? [], id: \.self) { Notice(text: $0, tone: .warn) }
        if let o = m.outputWarning, !m.destination.isIOS { Notice(text: o, tone: .warn) }
        if let n = m.inputNote { Notice(text: n, tone: .info) }
    }

    @ViewBuilder private var include: some View {
        if let p = m.plan, let b = p.base {
            VStack(alignment: .leading, spacing: 8) {
                SectionLabel("Include")
                RowGroup {
                    includeRow(title: String(localized: "Base game"), size: b.size, isOn: .constant(true), enabled: false, divider: !(p.updates.isEmpty && p.dlc.isEmpty))
                    ForEach(Array(p.updates.enumerated()), id: \.element.id) { i, u in
                        includeRow(title: String(localized: "Update \(u.versionLabel)"), size: u.size,
                                   isOn: Binding(get: { u.selected }, set: { _ in m.toggle(updates: true, id: u.id) }), enabled: true,
                                   divider: i < p.updates.count - 1 || !p.dlc.isEmpty)
                    }
                    ForEach(Array(p.dlc.enumerated()), id: \.element.id) { i, d in
                        includeRow(title: String(localized: "DLC \(d.titleId)"), size: d.size,
                                   isOn: Binding(get: { d.selected }, set: { _ in m.toggle(updates: false, id: d.id) }), enabled: true, divider: i < p.dlc.count - 1)
                    }
                }
                if !(p.updates.isEmpty && p.dlc.isEmpty) {
                    Text("\(p.includedSummary) · \(PlanResult.formatBytes(p.totalSelectedBytes)) in total").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private func includeRow(title: String, size: UInt64, isOn: Binding<Bool>, enabled: Bool, divider: Bool) -> some View {
        VStack(spacing: 0) {
            Group {
                if enabled {
                    Toggle(isOn: isOn) { includeLabel(title, size) }.toggleStyle(.checkbox)
                } else {
                    // always included: shown as a ticked line, not as a dimmed control
                    HStack(spacing: 6) {
                        Image(systemName: "checkmark.square.fill").foregroundStyle(.secondary)
                        includeLabel(title, size)
                    }
                    .accessibilityElement(children: .combine).accessibilityLabel(Text("\(title), always included"))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 9)
            if divider { Divider().overlay(Theme.hairline).padding(.leading, 12) }
        }
    }

    private func includeLabel(_ title: String, _ size: UInt64) -> some View {
        HStack {
            Text(title).lineLimit(1)
            Spacer()
            Text(PlanResult.formatBytes(size)).foregroundStyle(.secondary).monospacedDigit()
        }
    }

    // MARK: Mac

    private var macOptions: some View {
        VStack(alignment: .leading, spacing: 8) {
            RowGroup {
                RowLine(label: "Save to") {
                    Text(m.outputDir.path.replacingOccurrences(of: FileManager.default.homeDirectoryForCurrentUser.path, with: "~"))
                        .font(.system(size: 11.5, design: .monospaced)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    Button("Change…") {
                        if let d = pick(files: false, dirs: true, message: String(localized: "Choose where built apps go")).first { m.outputDir = d }
                    }
                    .buttonStyle(.glass).controlSize(.small)
                }
                RowLine(label: "Play when the build finishes", divider: false) {
                    Toggle("", isOn: $playAfter).labelsHidden().toggleStyle(.switch).controlSize(.small)
                }
            }
            Text("Added to your library when done. Keep the folder outside Desktop and Documents so the signature stays valid.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: iOS

    @ViewBuilder private var iosOptions: some View {
        @Bindable var m = m
        VStack(alignment: .leading, spacing: 8) {
            RowGroup {
                RowLine(label: "Install to", divider: true) {
                    Picker("Install to", selection: $m.destination) {
                        ForEach(m.installTargets) { t in Text(t.detail.map { "\(t.title) · \($0)" } ?? t.title).tag(t.destination) }
                    }
                    .labelsHidden().pickerStyle(.menu).fixedSize().accessibilityIdentifier("plan.installTo")
                }
                if !m.target.isSimulator {
                    RowLine(label: "Signing") {
                        Text(signingSummary).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
                        Button("Change…") { m.settingsTab = "devices"; openSettings() }.buttonStyle(.glass).controlSize(.small)
                    }
                }
                RowLine(label: "Bundle id") {
                    Text(m.effectiveBundleId).font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                }
                if case .device = m.destination {
                    RowLine(label: "Launch after install", divider: !m.target.isSimulator) {
                        Toggle("", isOn: $m.iosLaunchAfterInstall).labelsHidden().toggleStyle(.switch).controlSize(.small)
                    }
                }
                if !m.target.isSimulator {
                    RowLine(label: "Debug build", divider: false) {
                        Toggle("", isOn: $m.iosDebugBuild).labelsHidden().toggleStyle(.switch).controlSize(.small)
                    }
                }
            }
            if let note = m.deviceNote, m.devices.isEmpty { Text(note).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
            Text("iPhone or iPad is detected from the device. Choose an .ipa export in the list to build a file instead of installing.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if m.target.isSimulator {
                Text("Simulator build: ad-hoc signed, installed with simctl. Nothing to sign.").font(.caption).foregroundStyle(.secondary)
            } else if m.signingMode != .unsigned {
                Notice(text: m.entitlementPlan.explanation, tone: m.entitlementPlan.includesMemory ? .info : .warn)
            }
            if let n = m.bundleIdNote { Notice(text: n, tone: .info) }
        }
    }

    private var signingSummary: String {
        switch m.signingMode {
        case .automatic: m.selectedTeam.map { "\(String(localized: "Automatic")) · \($0.name)" } ?? String(localized: "Automatic · no team")
        case .profile: m.selectedProfile.map { "\(String(localized: "Profile")) · \($0.name)" } ?? String(localized: "Provisioning profile")
        case .unsigned: String(localized: "Unsigned")
        }
    }
}
