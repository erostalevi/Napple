import SwiftUI
import AppKit
import NappleStudioCore

/// Settings tabs for the shared system data. Everything lands in <support>/system.json, which the runtime reads at boot (NappleCLI.swift: SystemConfig).
@MainActor
@Observable
final class SystemSettingsModel {
    var config = SystemConfig.load()
    var message: String?
    func commit() { do { try config.save(); message = nil } catch { message = error.localizedDescription } }
    var mii: MiiStore { MiiStore(folder: config.effectiveMiiFolder) }
}

private struct PathRow: View {
    let title: LocalizedStringKey
    @Binding var value: String
    var folder = false
    var placeholder = ""
    var onCommit: () -> Void
    var body: some View {
        LabeledContent(title) {
            HStack {
                TextField("", text: $value, prompt: Text(placeholder)).textFieldStyle(.roundedBorder).onSubmit(onCommit)
                Button("Choose…") {
                    if let u = pick(files: !folder, dirs: folder, message: String(localized: "Choose")).first { value = u.path; onCommit() }
                }
            }
        }
    }
}

struct MiiSettings: View {
    @State private var s = SystemSettingsModel()
    @State private var entries: [MiiStore.Entry] = []
    @State private var note: String?
    var body: some View {
        Form {
            Section("Shared Mii database") {
                LabeledContent("Folder") { Text(s.mii.folder.path).textSelection(.enabled).lineLimit(1).truncationMode(.middle) }
                PathRow(title: "Use another folder", value: $s.config.miiDir, folder: true, placeholder: "default (system/mii)") { s.commit(); reload() }
                if entries.isEmpty { Text("No Mii yet. The database is created the first time a game asks for it.").foregroundStyle(.secondary) }
                ForEach(entries, id: \.index) { e in
                    LabeledContent(e.name.isEmpty ? "Mii \(e.index + 1)" : e.name) {
                        Button("Delete", role: .destructive) { do { try s.mii.delete(index: e.index); reload() } catch { note = error.localizedDescription } }
                            .disabled(entries.count < 2)
                    }
                }
            }
            HStack {
                Button("Back Up") { do { note = String(localized: "Saved \(try s.mii.backup().lastPathComponent)"); reload() } catch { note = error.localizedDescription } }
                Button("Export…") {
                    if let d = pick(files: false, dirs: true, message: String(localized: "Choose a folder for MiiDatabase.dat")).first { do { try s.mii.export(to: d); note = String(localized: "Exported.") } catch { note = error.localizedDescription } }
                }
                Button("Restore…") {
                    if let f = pick(files: true, dirs: false, message: String(localized: "Choose a Mii database backup")).first { do { try s.mii.restore(from: f); reload() } catch { note = error.localizedDescription } }
                }
                Button("Open Folder") { try? FileManager.default.createDirectory(at: s.mii.folder, withIntermediateDirectories: true); NSWorkspace.shared.open(s.mii.folder) }
            }
            if let n = note ?? s.message { Text(n).font(.caption).foregroundStyle(.secondary) }
        }
        .formStyle(.grouped)
        .onAppear(perform: reload)
    }
    private func reload() { entries = s.mii.entries() }
}

struct AmiiboSettings: View {
    @State private var s = SystemSettingsModel()
    var body: some View {
        Form {
            Section("Amiibo (the data stays on this Mac; nothing is bundled)") {
                PathRow(title: "Amiibo list", value: $s.config.amiiboDatabase, placeholder: "URL, .json file or folder (default: community list)") { s.commit() }
                PathRow(title: "Key file", value: $s.config.amiiboKey, placeholder: "key_retail.bin") { s.commit() }
                PathRow(title: "Virtual amiibo folder", value: $s.config.amiiboStore, folder: true, placeholder: "default (system/amiibo)") { s.commit() }
            }
            Text("You can also change these inside a running game: F1 > Amiibo. Environment variables NAPPLE_AMIIBO_DB, NAPPLE_AMIIBO_KEY and NAPPLE_AMIIBO_STORE override this file.")
                .font(.caption).foregroundStyle(.secondary)
            if let n = s.message { Text(n).font(.caption).foregroundStyle(.orange) }
        }
        .formStyle(.grouped)
    }
}

struct OnlineSettings: View {
    @State private var s = SystemSettingsModel()
    var body: some View {
        Form {
            Section("Online provider") {
                PathRow(title: "Provider module", value: $s.config.onlineProvider, placeholder: "none: offline (NullProvider)") { s.commit() }
                if !s.config.onlineProviderUsable {
                    Label("That file cannot be read: games start offline with the NullProvider.", systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange)
                }
            }
            Text("Napple ships no online service. A provider is a library you supply; with none (or a broken one) every game runs offline. NAPPLE_ONLINE_PROVIDER overrides this setting.")
                .font(.caption).foregroundStyle(.secondary)
            if let n = s.message { Text(n).font(.caption).foregroundStyle(.orange) }
        }
        .formStyle(.grouped)
    }
}

/// Per-game Run view: starts the built app through its launcher and shows the runtime log live.
struct RunLogView: View {
    let item: LibraryItem
    @Environment(\.dismiss) private var dismiss
    @State private var lines: [String] = []
    @State private var run: GameRun?
    @State private var status = "Starting…"
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(item.entry.name).font(.title3.weight(.semibold))
                Spacer()
                Text(status).font(.caption).foregroundStyle(.secondary)
            }
            ScrollViewReader { proxy in
                ScrollView {
                    Text(lines.suffix(2000).joined(separator: "\n")).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Color.clear.frame(height: 1).id("end")
                }
                .onChange(of: lines.count) { proxy.scrollTo("end") }
            }
            .padding(8).background(.quaternary, in: .rect(cornerRadius: 10))
            HStack {
                Button("Copy Log") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(lines.joined(separator: "\n"), forType: .string) }
                Spacer()
                Button(run?.isRunning == true ? "Stop" : "Close") { run?.stop(); dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(18).frame(minWidth: 720, minHeight: 440)
        .task {
            guard let url = item.url else { status = "The app is missing."; return }
            let r = GameRun(app: url); run = r
            do {
                try r.start(onLine: { l in Task { @MainActor in lines.append(l) } },
                            onExit: { c in Task { @MainActor in status = c == 0 ? "Exited normally" : "Exited with code \(c)" } })
                status = "Running"
            } catch { status = error.localizedDescription }
        }
        .onDisappear { run?.stop() }
    }
}
