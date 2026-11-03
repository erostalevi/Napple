import SwiftUI
import AppKit
import NappleStudioCore

/// The right-hand panel: one place for the whole Add / Build flow, switching on the build phase.
struct PanelHost: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Group {
            switch m.phase {
            case .idle, .planning: ReadingPanel()
            case .ready: PlanPanel()
            case .building: BuildPanel()
            case .done: DonePanel()
            case .failed: FailedPanel()
            }
        }
        .frame(width: Theme.panelWidth)
        .frame(maxHeight: .infinity)
        .background(.regularMaterial)
        .overlay(alignment: .leading) { Rectangle().fill(Theme.hairline).frame(width: 1) }
        .shadow(color: .black.opacity(0.28), radius: 18, x: -6, y: 0)
        .animation(.smooth(duration: 0.25), value: m.phase)
    }
}

/// Header, scrolling content, footer buttons: the frame every panel state shares.
struct PanelFrame<Header: View, Content: View, Footer: View>: View {
    @ViewBuilder var header: Header
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer
    var body: some View {
        VStack(spacing: 0) {
            header.padding(.horizontal, 22).padding(.top, 22).padding(.bottom, 14)
            ScrollView {
                VStack(alignment: .leading, spacing: 16) { content }
                    .padding(.horizontal, 22).padding(.bottom, 14)
            }
            .scrollIndicators(.automatic)
            Divider().overlay(Theme.hairline)
            HStack(spacing: 10) { footer }.padding(.horizontal, 22).padding(.vertical, 14)
        }
    }
}

/// Cover, name, one status line, and an optional trailing view.
struct PanelHeader<Trailing: View>: View {
    @Environment(AppModel.self) private var m
    let title: String
    var subtitle: String?
    var subtitleColor: Color = .secondary
    var skeleton = false
    @ViewBuilder var trailing: Trailing
    var body: some View {
        HStack(spacing: 14) {
            Group {
                if skeleton { RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Theme.surfaceStrong) }
                else { CoverView(image: nil, name: title, seed: m.plan?.base?.titleId ?? title).clipShape(.rect(cornerRadius: 12, style: .continuous)) }
            }
            .frame(width: 56, height: 56)
            VStack(alignment: .leading, spacing: 3) {
                if skeleton {
                    RoundedRectangle(cornerRadius: 4).fill(Theme.surfaceStrong).frame(width: 150, height: 16)
                    RoundedRectangle(cornerRadius: 4).fill(Theme.surface).frame(width: 210, height: 12)
                } else {
                    Text(title).font(.system(size: 17, weight: .semibold)).lineLimit(2)
                    if let subtitle { Text(subtitle).font(.caption).foregroundStyle(subtitleColor).monospacedDigit().lineLimit(1) }
                }
            }
            Spacer(minLength: 6)
            trailing
        }
    }
}
extension PanelHeader where Trailing == EmptyView {
    init(title: String, subtitle: String? = nil, subtitleColor: Color = .secondary, skeleton: Bool = false) {
        self.init(title: title, subtitle: subtitle, subtitleColor: subtitleColor, skeleton: skeleton) { EmptyView() }
    }
}

/// Compatibility status as a dot and a word.
struct CompatBadge: View {
    let entry: CompatEntry
    var body: some View {
        HStack(spacing: 6) { StatusDot(color: entry.status.color); Text(entry.status.label).font(.caption) }
            .accessibilityElement(children: .combine)
    }
}

// MARK: reading

/// Between dropping the files and the plan: nothing to decide yet.
struct ReadingPanel: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        PanelFrame {
            PanelHeader(title: "", skeleton: true)
        } content: {
            HStack(spacing: 12) {
                ProgressView().controlSize(.small)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Reading your game files…").fontWeight(.semibold)
                    Text("Checking the base game, updates and DLC.").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(14).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            VStack(alignment: .leading, spacing: 8) {
                SectionLabel("Files")
                RowGroup {
                    ForEach(Array(m.files.enumerated()), id: \.offset) { i, f in
                        RowLine(label: LocalizedStringKey(f.lastPathComponent), divider: i < m.files.count - 1) {
                            Text(Self.size(of: f)).foregroundStyle(.secondary).monospacedDigit()
                        }
                    }
                }
            }
        } footer: {
            Button("Cancel") { m.closePanel() }.controlSize(.large).keyboardShortcut(.cancelAction)
            Spacer()
            Button("Build") {}.buttonStyle(.glassProminent).controlSize(.large).disabled(true)
        }
    }
    static func size(of url: URL) -> String {
        let v = try? url.resourceValues(forKeys: [.fileSizeKey, .isDirectoryKey])
        if v?.isDirectory == true { return String(localized: "Folder") }
        return v?.fileSize.map { PlanResult.formatBytes(UInt64($0)) } ?? ""
    }
}

// MARK: shared log

/// The build log as a dark monospaced well that follows the newest line.
struct LogWell: View {
    let lines: [String]
    var filter = ""
    var body: some View {
        let shown = filter.isEmpty ? lines : lines.filter { $0.localizedCaseInsensitiveContains(filter) }
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(shown.suffix(600).enumerated()), id: \.offset) { i, l in
                        Text(l).font(.system(size: 11, design: .monospaced)).foregroundStyle(Self.tone(l)).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading).id(i)
                    }
                    Color.clear.frame(height: 1).id("end")
                }
                .padding(.horizontal, 10).padding(.vertical, 8)
            }
            .onChange(of: shown.count) { if filter.isEmpty { proxy.scrollTo("end", anchor: .bottom) } }
            .onAppear { proxy.scrollTo("end", anchor: .bottom) }
        }
        .background(.black.opacity(0.30), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        .overlay { if lines.isEmpty { Text("Build output appears here, line by line.").font(.caption).foregroundStyle(.tertiary) } }
    }
    static func tone(_ l: String) -> Color {
        if l.hasPrefix("$ ") { return .accentColor }
        switch LogLevel.classify(l) {
        case .error: return Theme.bad
        case .warning: return Theme.warn
        case .info: return .primary
        }
    }
}
