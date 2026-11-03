import SwiftUI
import AppKit
import NappleStudioCore

private func copyToPasteboard(_ text: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
}

// MARK: building

struct BuildPanel: View {
    @Environment(AppModel.self) private var m
    @State private var filter = ""

    var body: some View {
        PanelFrame {
            PanelHeader(title: m.planTitle, subtitle: String(localized: "Building for \(m.destinationLabel)"))
        } content: {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .firstTextBaseline) {
                    Text(AppModel.stageName(m.progress?.stage ?? "preflight")).fontWeight(.semibold).contentTransition(.opacity)
                    Spacer()
                    if let s = m.buildStarted { Text(s, style: .timer).monospacedDigit().foregroundStyle(.secondary) }
                    Text("\(Int(m.progress?.percent ?? 0))%").font(.title3.weight(.semibold)).monospacedDigit()
                        .contentTransition(.numericText(value: m.progress?.percent ?? 0)).frame(minWidth: 52, alignment: .trailing)
                }
                ProgressView(value: m.buildFraction).progressViewStyle(.linear).animation(.smooth(duration: 0.25), value: m.buildFraction)
                Text(m.progress?.message ?? "").font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    .accessibilityIdentifier("build.progress")
                Text("This can take a few minutes. You can keep using your Mac.").font(.caption).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    SectionLabel("Log")
                    Spacer()
                    TextField("Filter", text: $filter).textFieldStyle(.roundedBorder).controlSize(.small).frame(width: 120)
                    Button("Copy") { copyToPasteboard(m.log.joined(separator: "\n")) }.buttonStyle(.glass).controlSize(.small)
                }
                LogWell(lines: m.log, filter: filter).frame(height: 230)
            }
        } footer: {
            Button("Close") { m.closePanel() }.controlSize(.large).keyboardShortcut(.cancelAction)
            Spacer()
            Button(role: .destructive) { m.cancelBuild() } label: { Text(m.cancelling ? "Cancelling…" : "Cancel Build").foregroundStyle(Theme.bad) }
                .buttonStyle(.glass).controlSize(.large).disabled(m.cancelling).accessibilityIdentifier("build.cancel")
        }
    }
}

// MARK: done

struct DonePanel: View {
    @Environment(AppModel.self) private var m

    private var isMac: Bool { m.target == .macos }
    private var shownNotes: [String] {
        m.notes.map { $0.replacingOccurrences(of: "^(note|warning): ?", with: "", options: .regularExpression) }
    }

    var body: some View {
        PanelFrame {
            PanelHeader(title: m.planTitle, subtitle: headerLine, subtitleColor: m.installFailed ? Theme.bad : Theme.good)
        } content: {
            HStack(spacing: 14) {
                Image(systemName: "checkmark.circle").font(.system(size: 30, weight: .regular)).foregroundStyle(Theme.good).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    if let d = m.buildDuration { Text("Built in \(Duration.seconds(d).formatted(.units(allowed: [.minutes, .seconds], width: .abbreviated)))").fontWeight(.semibold) }
                    Text([m.builtBytes.map { PlanResult.formatBytes($0) }, m.destinationLabel].compactMap { $0 }.joined(separator: " · ")).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(14).background(Theme.good.opacity(0.14), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))

            if let p = m.builtIPA ?? m.builtApp {
                VStack(alignment: .leading, spacing: 4) {
                    Text(m.builtIPA != nil ? "Saved as" : "Saved to").font(.caption).foregroundStyle(.secondary)
                    Text(p.path).font(.system(size: 11.5, design: .monospaced)).textSelection(.enabled).lineLimit(3).truncationMode(.middle)
                }
                .padding(.horizontal, 12).padding(.vertical, 9).frame(maxWidth: .infinity, alignment: .leading)
                .background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            }

            if !isMac, case .device(let id) = m.destination { installSection(deviceName: m.devices.first { $0.id == id }?.name ?? String(localized: "the device")) }

            if !shownNotes.isEmpty {
                VStack(alignment: .leading, spacing: 8) {
                    SectionLabel("Notes")
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(Array(shownNotes.enumerated()), id: \.offset) { _, n in Text(n).font(.callout).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true) }
                    }
                    .padding(12).frame(maxWidth: .infinity, alignment: .leading).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
                }
            }
        } footer: {
            Button("Close") { m.closePanel() }.controlSize(.large).keyboardShortcut(.cancelAction).accessibilityIdentifier("done.close")
            Button("Show in Finder") { m.reveal(m.builtIPA ?? m.builtApp) }.buttonStyle(.glass).controlSize(.large)
            Spacer()
            primary
        }
    }

    private var headerLine: String {
        if isMac { return String(localized: "Ready to play") }
        if m.installFailed { return String(localized: "Install failed") }
        if m.installing { return String(localized: "Installing…") }
        if case .device = m.destination, m.installMessage != nil { return String(localized: "Installed") }
        return String(localized: "Ready")
    }

    @ViewBuilder private var primary: some View {
        if isMac {
            Button { m.launch(m.builtApp) } label: { Label("Play", systemImage: "play.fill") }
                .buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("done.play")
        } else if case .device(let id) = m.destination, let dev = m.devices.first(where: { $0.id == id }) {
            Button { Task { await m.installToDevice() } } label: {
                Label(m.installing ? "Installing…" : (m.installFailed ? "Try Install Again" : "Install on \(dev.name)"), systemImage: "iphone.and.arrow.forward")
            }
            .buttonStyle(.glassProminent).controlSize(.large).disabled(m.installing).accessibilityIdentifier("done.install")
        }
    }

    @ViewBuilder private func installSection(deviceName: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Install")
            if m.installing {
                HStack(spacing: 12) {
                    ProgressView().controlSize(.small)
                    Text(m.installMessage ?? String(localized: "Installing on \(deviceName)…")).font(.callout)
                    Spacer()
                }
                .padding(14).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
                Text("Keep the device unlocked and connected. The app launches by itself when the install finishes.").font(.caption).foregroundStyle(.secondary)
            } else if let msg = m.installMessage {
                Notice(text: msg, tone: m.installFailed ? .bad : .good)
            } else {
                Notice(text: String(localized: "Built, not installed yet. Press Install on \(deviceName)."), tone: .info)
            }
        }
    }
}

// MARK: failed

struct FailedPanel: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        PanelFrame {
            PanelHeader(title: m.plan == nil ? String(localized: "Your game") : m.planTitle,
                        subtitle: m.plan == nil ? String(localized: "Could not read the game") : String(localized: "The build did not finish"), subtitleColor: Theme.bad)
        } content: {
            Notice(text: m.errorText ?? String(localized: "Something went wrong."), tone: .bad)
            VStack(alignment: .leading, spacing: 8) {
                SectionLabel("Last lines of the log")
                LogWell(lines: Array(m.log.suffix(12))).frame(height: 170)
            }
        } footer: {
            Button("Close") { m.closePanel() }.controlSize(.large).keyboardShortcut(.cancelAction).accessibilityIdentifier("failed.close")
            Button("Copy Log") { copyToPasteboard(m.log.joined(separator: "\n")) }.buttonStyle(.glass).controlSize(.large)
            Spacer()
            if m.plan != nil {
                Button("Try Again") { m.backToPlan() }.buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("failed.retry")
            } else {
                Button("Choose Files…") { m.resetPlanState(); m.pickFilesAndAdd() }.buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction)
            }
        }
    }
}
