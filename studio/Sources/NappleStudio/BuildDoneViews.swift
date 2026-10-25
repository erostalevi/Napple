import SwiftUI
import AppKit
import NappleStudioCore

/// Live build progress: bar, current file, stage chips, log tail. Also where a failed build shows its error.
struct BuildingView: View {
    @Environment(AppModel.self) private var m
    @AppStorage("showBuildLog") private var showLog = true

    /// The CLI's stages grouped into the four the user sees.
    private static let groups: [(String, [String])] = [
        ("Read", ["preflight", "ingest", "plan"]),
        ("Convert", ["rewrite"]),
        ("Pack", ["romfs", "dlc", "fonts", "bundle"]),
        ("Sign", ["sign", "done"]),
    ]

    private var activeGroup: Int {
        guard let s = m.progress?.stage else { return 0 }
        return Self.groups.firstIndex { $0.1.contains(s) } ?? 0
    }

    var body: some View {
        if m.phase == .failed { failed } else { running }
    }

    private var running: some View {
        VStack(spacing: 22) {
            Spacer(minLength: 0)
            ScreenTitle(title: "Building \(m.planTitle)…", subtitle: m.cancelling ? "Stopping…" : "Converting code and packing assets. Keep this window open.",
                        size: 32, centered: true)
                .frame(maxWidth: .infinity)
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(m.progress?.stage.capitalized ?? "Starting").font(.system(size: 14, weight: .semibold))
                    Spacer()
                    Text("\(Int(m.progress?.percent ?? 0))%").font(.system(size: 14)).foregroundStyle(Palette.secondary).monospacedDigit()
                }
                GeometryReader { g in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Palette.fillStrong)
                        Capsule().fill(Palette.progress).frame(width: max(8, g.size.width * CGFloat((m.progress?.percent ?? 0) / 100)))
                    }
                }
                .frame(height: 10)
                .accessibilityElement()
                .accessibilityLabel("Build progress")
                .accessibilityValue("\(Int(m.progress?.percent ?? 0)) percent")
                Text(m.progress?.message ?? "").font(.system(size: 12, design: .monospaced)).foregroundStyle(Palette.tertiary)
                    .lineLimit(1).truncationMode(.middle)
            }
            HStack(spacing: 10) {
                ForEach(Array(Self.groups.enumerated()), id: \.offset) { i, g in chip(g.0, index: i) }
            }
            VStack(spacing: 10) {
                if showLog { logPanel }
                HStack {
                    Button { showLog.toggle() } label: { Label(showLog ? "Hide logs" : "Show logs", systemImage: "terminal") }
                        .buttonStyle(SecondaryPill(height: 32))
                    Spacer()
                    Button("Cancel") { m.cancelBuild() }.buttonStyle(SecondaryPill(height: 40, fontSize: 14))
                        .disabled(m.cancelling).keyboardShortcut(.cancelAction)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 60).padding(.bottom, 20)
    }

    private func chip(_ title: String, index: Int) -> some View {
        let done = index < activeGroup, active = index == activeGroup
        return HStack(spacing: 8) {
            if done { Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(Palette.red) }
            else if active { Circle().fill(Palette.red).frame(width: 8, height: 8) }
            Text(title).font(.system(size: 13, weight: active ? .semibold : .regular))
                .foregroundStyle(done || active ? Color.white : Palette.tertiary)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14).frame(height: 40)
        .background(active ? Palette.fillStrong : Palette.fill, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(active ? Palette.red.opacity(0.8) : Palette.stroke))
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(title): \(done ? "done" : active ? "in progress" : "waiting")")
    }

    private var logPanel: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Array(m.log.suffix(5).enumerated()), id: \.offset) { i, l in
                Text(l).font(.system(size: 12, design: .monospaced)).lineLimit(1).truncationMode(.tail)
                    .foregroundStyle(i == min(4, m.log.count - 1) ? Color.white : Palette.secondary)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 112, alignment: .bottomLeading)
        .padding(.horizontal, 14).padding(.vertical, 10)
        .background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Palette.stroke))
        .accessibilityLabel("Build log, latest lines")
    }

    private var failed: some View {
        VStack(spacing: 14) {
            Spacer(minLength: 0)
            Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 36)).foregroundStyle(Palette.danger)
            Text("The build stopped").font(.system(size: 26, weight: .medium, design: .serif))
            Text(m.errorText ?? "Unknown error.").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                .multilineTextAlignment(.center).frame(maxWidth: 560).textSelection(.enabled)
            if !m.log.isEmpty { logPanel.frame(maxWidth: 600) }
            HStack(spacing: 10) {
                Button("Try again") { m.startBuild() }.buttonStyle(PrimaryPill(height: 40)).keyboardShortcut(.defaultAction)
                Button("Back to plan") { m.backToPlan() }.buttonStyle(SecondaryPill(height: 40, fontSize: 13))
                Button("Show log in Finder") { m.showLogsInFinder() }.buttonStyle(SecondaryPill(height: 40, fontSize: 13))
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 60).padding(.bottom, 20)
    }
}

/// The app is built: launch it, find it, or put it in /Applications (or on a device for iPhone and iPad builds).
struct DoneView: View {
    @Environment(AppModel.self) private var m

    private var isIOS: Bool { m.target.isIOS }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            hero
            HStack(spacing: 12) {
                stat("App size", sizeText)
                stat("Included", m.builtSummary.isEmpty ? "Base game only" : m.builtSummary)
                stat("Built for", m.destinationLabel)
                stat("Build time", durationText)
            }
            if !m.notes.isEmpty { notesCard }
            if isIOS { iosCard } else { installCard }
            Spacer(minLength: 0)
            HStack(alignment: .center) {
                Button("Build another game") { m.startOver() }.buttonStyle(.plain)
                    .font(.system(size: 14)).foregroundStyle(Palette.secondary).underline()
                Spacer()
                LegalFooter().frame(maxWidth: 420)
            }
        }
        .padding(.horizontal, 40).padding(.top, 14).padding(.bottom, 14)
    }

    private var hero: some View {
        HStack(spacing: 20) {
            AppIconView(url: m.builtApp, size: 112, radius: 16)
            VStack(alignment: .leading, spacing: 10) {
                VStack(alignment: .leading, spacing: 4) {
                    Label("Your app is ready", systemImage: "checkmark").font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.mint)
                    Text(m.builtInfo?.name ?? m.planTitle).font(.system(size: 36, weight: .medium, design: .serif)).lineLimit(1).minimumScaleFactor(0.6)
                    Text(m.builtApp?.path ?? "").font(.system(size: 12, design: .monospaced)).foregroundStyle(Palette.tertiary)
                        .lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                }
                HStack(spacing: 10) {
                    if isIOS {
                        Button { m.installToDevice() } label: { Label("Install on \(m.selectedDevice?.name ?? "device")", systemImage: "iphone") }
                            .buttonStyle(PrimaryPill(height: 44, fontSize: 15))
                            .disabled(m.builtApp == nil || m.selectedDevice == nil || m.installing || m.iosUnsigned || m.destination == .ipaIPhone || m.destination == .ipaIPad)
                        Button("Show in Finder") { m.reveal() }.buttonStyle(SecondaryPill(height: 44, fontSize: 14))
                        if let ipa = m.builtIPA {
                            Button(".ipa") { NSWorkspace.shared.activateFileViewerSelecting([ipa]) }.buttonStyle(SecondaryPill(height: 44, fontSize: 14))
                        }
                    } else {
                        Button { m.launch() } label: { Label("Launch", systemImage: "play.fill") }
                            .buttonStyle(PrimaryPill(height: 44, fontSize: 15)).keyboardShortcut(.defaultAction)
                            .disabled(m.builtApp == nil)
                        Button("Show in Finder") { m.reveal() }.buttonStyle(SecondaryPill(height: 44, fontSize: 14))
                    }
                }
            }
            Spacer(minLength: 0)
        }
        .card(radius: 36, padding: 20)
    }

    private func stat(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            SectionLabel(label)
            Text(value).font(.system(size: 17, weight: .semibold)).lineLimit(2).minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity, minHeight: 56, alignment: .topLeading)
        .card(radius: 14, padding: 14)
        .accessibilityElement(children: .combine)
    }

    private var notesCard: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(m.notes.prefix(3)), id: \.self) { Text($0).font(.system(size: 12)).foregroundStyle(Palette.amber).lineLimit(2) }
            if m.notes.count > 3 { Text("and \(m.notes.count - 3) more in the log.").font(.system(size: 12)).foregroundStyle(Palette.tertiary) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .card(radius: 14, padding: 12, tint: Palette.amber.opacity(0.1))
    }

    private var installCard: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(m.installMessage ?? "Keep it handy").font(.system(size: 14, weight: .semibold))
                Text(m.installMessage == nil ? "Copy the app into your Applications folder so it shows up in Launchpad. It also stays in your library." : "It also stays in your library.")
                    .font(.system(size: 13)).foregroundStyle(Palette.secondary)
            }
            Spacer()
            Button("Install to Applications") { m.installToApplications() }
                .buttonStyle(SecondaryPill(height: 40, fontSize: 14)).disabled(m.builtApp == nil)
        }
        .card(radius: 14, padding: 14)
    }

    private var iosCard: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(m.installMessage ?? (m.iosUnsigned ? "Unsigned build" : "Install on your device")).font(.system(size: 14, weight: .semibold))
            Text(m.iosUnsigned
                 ? "Sign the .ipa with your own tool (AltStore, Sideloadly, …) before installing it."
                 : "Needs Developer Mode on the device. A free Apple ID profile expires after 7 days.")
                .font(.system(size: 13)).foregroundStyle(Palette.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .card(radius: 14, padding: 14)
    }

    private var sizeText: String { m.builtBytes.map { PlanResult.formatBytes($0) } ?? "…" }

    private var durationText: String {
        guard let d = m.buildDuration else { return "—" }
        let s = Int(d.rounded())
        return s >= 60 ? "\(s / 60) min \(s % 60) s" : "\(s) s"
    }
}
