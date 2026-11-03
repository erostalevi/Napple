import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// "Play with Debug": the game's live log in its own window. The game itself opens as usual.
struct ConsoleWindow: View {
    @Environment(AppModel.self) private var m
    let id: UUID
    @State private var level = DebugConsole.Level.all
    @State private var filter = ""
    @State private var follow = true

    var body: some View {
        if let c = m.consoles.first(where: { $0.id == id }) { content(c) }
        else { ContentUnavailableView("This console is closed", systemImage: "terminal").frame(width: 680, height: 500) }
    }

    private func content(_ c: DebugConsole) -> some View {
        let shown = c.shown(level: level, filter: filter)
        return VStack(spacing: 10) {
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Circle().fill(c.running ? Theme.good : .secondary).frame(width: 8, height: 8)
                    Text(c.name).fontWeight(.semibold)
                    Text(c.statusText).foregroundStyle(.secondary)
                    if c.running, let s = c.startedAt { Text(s, style: .timer).monospacedDigit().foregroundStyle(.secondary) }
                }
                Spacer()
                TextField("Filter", text: $filter).textFieldStyle(.roundedBorder).frame(width: 150)
                Picker("Level", selection: $level) {
                    Text("All").tag(DebugConsole.Level.all); Text("Warnings").tag(DebugConsole.Level.warnings); Text("Errors").tag(DebugConsole.Level.errors)
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 190)
            }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(shown, id: \.offset) { row in
                            Text(row.line).font(.system(size: 11.5, design: .monospaced)).foregroundStyle(Self.color(row.line)).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        Color.clear.frame(height: 1).id("end")
                    }
                    .padding(.horizontal, 12).padding(.vertical, 8)
                }
                .onChange(of: shown.count) { if follow { proxy.scrollTo("end", anchor: .bottom) } }
            }
            .background(.black.opacity(0.30), in: .rect(cornerRadius: 8, style: .continuous))
            .overlay { if shown.isEmpty { Text(c.lines.isEmpty ? "Waiting for the game's first line…" : "Nothing matches the filter.").foregroundStyle(.tertiary) } }
            HStack(spacing: 8) {
                Toggle("Follow live", isOn: $follow).toggleStyle(.checkbox)
                Spacer()
                Button("Clear") { c.clear() }.buttonStyle(.glass)
                Button("Copy") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(shown.map(\.line).joined(separator: "\n"), forType: .string) }.buttonStyle(.glass)
                Button("Save…") { save(c) }.buttonStyle(.glass)
                if c.running {
                    Button("Stop Game") { c.stop() }.buttonStyle(.glass).tint(Theme.bad)
                } else {
                    Button("Play Again") { c.start() }.buttonStyle(.glassProminent)
                }
            }
        }
        .padding(16)
        .frame(width: 680, height: 500)
        .background(WindowMaterial().ignoresSafeArea())
        .navigationTitle("Console · \(c.name)")
    }

    private func save(_ c: DebugConsole) {
        let p = NSSavePanel()
        p.nameFieldStringValue = "\(c.name) log.txt"
        p.allowedContentTypes = [.plainText]
        guard p.runModal() == .OK, let url = p.url else { return }
        do { try c.save(to: url) } catch { m.say(String(localized: "Could not save the log: \(error.localizedDescription)")) }
    }

    static func color(_ l: String) -> Color {
        switch LogLevel.classify(l) {
        case .error: Theme.bad
        case .warning: Theme.warn
        case .info: .primary
        }
    }
}
