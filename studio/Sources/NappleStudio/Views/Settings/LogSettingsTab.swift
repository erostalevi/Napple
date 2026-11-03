import SwiftUI
import AppKit
import NappleStudioCore

/// The last build's log: live while a build runs, else the newest log file on disk.
struct LogSettingsTab: View {
    @Environment(AppModel.self) private var m
    @State private var fileLines: [String] = []
    @State private var fileName: String?
    @State private var loading = false
    @State private var filter = ""
    @State private var follow = true
    @State private var copied = false

    private static let maxLines = 2000
    private var live: Bool { !m.log.isEmpty }
    private var lines: [String] { live ? Array(m.log.suffix(Self.maxLines)) : fileLines }
    private var shown: [String] {
        let f = filter.trimmingCharacters(in: .whitespaces)
        return f.isEmpty ? lines : lines.filter { $0.localizedCaseInsensitiveContains(f) }
    }
    private var gameName: String? {
        if let n = m.compat?.name, !n.isEmpty { return n }
        if let b = m.builtInfo?.name, !b.isEmpty { return b }
        if m.plan != nil { return m.planTitle }
        return nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                SettingsHeader("Last build")
                if let g = gameName { Text(g).font(.callout.weight(.medium)).lineLimit(1).accessibilityIdentifier("settings.log.game") }
                Spacer()
                if live && m.isBuilding { Label("Live", systemImage: "circle.fill").font(.caption).foregroundStyle(.green).labelStyle(.titleAndIcon) }
                else if !live, let fileName { Text(fileName).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle) }
            }
            HStack(spacing: 8) {
                TextField("Filter", text: $filter, prompt: Text("Filter")).textFieldStyle(.roundedBorder).labelsHidden()
                    .accessibilityLabel("Filter").accessibilityIdentifier("settings.log.filter")
                Toggle("Follow", isOn: $follow).toggleStyle(.checkbox).accessibilityIdentifier("settings.log.follow")
                Button { copy() } label: { Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc") }
                    .disabled(shown.isEmpty).accessibilityIdentifier("settings.log.copy")
                Button("Show Logs in Finder") { m.showLogsInFinder() }.accessibilityIdentifier("settings.log.finder")
            }
            .buttonStyle(.glass)

            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous).fill(.black.opacity(0.28))
                RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(.separator.opacity(0.6))
                if loading && lines.isEmpty {
                    ProgressView().controlSize(.small)
                } else if lines.isEmpty {
                    ContentUnavailableView {
                        Label("No build log yet", systemImage: "text.alignleft")
                    } description: { Text("Build a game and its log shows up here.") }
                    .accessibilityIdentifier("settings.log.empty")
                } else if shown.isEmpty {
                    ContentUnavailableView.search(text: filter)
                } else {
                    ScrollViewReader { proxy in
                        ScrollView {
                            Text(shown.joined(separator: "\n")).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading).padding(10)
                                .accessibilityIdentifier("settings.log.text")
                            Color.clear.frame(height: 1).id("end")
                        }
                        .onChange(of: shown.count) { if follow { proxy.scrollTo("end", anchor: .bottom) } }
                        .onChange(of: follow) { if follow { proxy.scrollTo("end", anchor: .bottom) } }
                        .onAppear { proxy.scrollTo("end", anchor: .bottom) }
                    }
                }
            }
        }
        .padding(.vertical, 14)
        .frame(maxWidth: 620)
        .frame(maxWidth: .infinity)
        .task { await loadFile() }
        .onChange(of: m.log.isEmpty) { _, empty in if empty { Task { await loadFile() } } }
    }

    private func loadFile() async {
        guard m.log.isEmpty else { return }
        loading = true
        let dir = AppModel.logsDir
        let (name, got) = await Task.detached { () -> (String?, [String]) in
            guard let f = AppModel.newestLogFile(in: dir) else { return (nil, []) }
            return (f.lastPathComponent, AppModel.tail(of: f, maxLines: 2000))
        }.value
        fileName = name; fileLines = got; loading = false
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(shown.joined(separator: "\n"), forType: .string)
        copied = true
        Task { try? await Task.sleep(nanoseconds: 1_500_000_000); copied = false }
    }
}
