import SwiftUI
import AppKit
import NappleStudioCore

/// The shared Mii database: list, delete one, back up, export, restore, open the folder, use another folder.
struct MiiSettingsTab: View {
    @State private var s = SettingsSystemModel()
    @State private var entries: [MiiStore.Entry] = []
    @State private var note: (text: String, tone: SettingsBanner.Tone)?
    @State private var pendingDelete: MiiStore.Entry?
    @State private var restoreFrom: URL?
    @State private var exists = false

    var body: some View {
        SettingsPage {
            Section {
                if entries.isEmpty {
                    ContentUnavailableView {
                        Label("No Mii yet", systemImage: "person.crop.square")
                    } description: {
                        Text(exists ? "The database could not be read. Restore a backup, or start a game to create a new one."
                                    : "The database is created the first time a game asks for it. You can also restore a backup.")
                    }
                    .frame(maxWidth: .infinity).accessibilityIdentifier("settings.mii.empty")
                } else {
                    ForEach(entries, id: \.index) { e in
                        LabeledContent(e.name.isEmpty ? String(localized: "Mii \(e.index + 1)") : e.name) {
                            Button("Delete", role: .destructive) { pendingDelete = e }
                                .disabled(entries.count < 2)
                                .help(entries.count < 2 ? String(localized: "The last Mii stays: a console always has one.") : "")
                                .accessibilityIdentifier("settings.mii.delete.\(e.index)")
                        }
                    }
                }
                if let note { SettingsBanner(text: note.text, tone: note.tone) }
            } header: { SettingsHeader("Shared Mii database") } footer: {
                SettingsFootnote("Every game shares this database. Deleting a Mii keeps the previous file as MiiDatabase.dat.bak. The last Mii cannot be deleted.")
            }

            Section {
                HStack(spacing: 8) {
                    Button("Back Up…") { backUp() }.disabled(entries.isEmpty).accessibilityIdentifier("settings.mii.backup")
                    Button("Export…") { export() }.disabled(entries.isEmpty).accessibilityIdentifier("settings.mii.export")
                    Button("Restore…") { chooseRestore() }.accessibilityIdentifier("settings.mii.restore")
                    Spacer()
                    Button("Open Folder") { openFolder() }.accessibilityIdentifier("settings.mii.openFolder")
                }
                .buttonStyle(.glass)
            }

            Section {
                LabeledContent("Folder") { Text(s.mii.folder.path).textSelection(.enabled).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary) }
                SettingsPathRow(title: "Use another folder", value: $s.config.miiDir, folder: true, placeholder: "default (system/mii)", identifier: "settings.mii.folder") {
                    s.commit(\.miiDir); reload()
                }
                if let m = s.message { SettingsBanner(text: m, tone: .bad) }
            } header: { SettingsHeader("Location") } footer: {
                SettingsFootnote("NAPPLE_MII_DIR overrides this setting.")
            }
        }
        .onAppear { s.reload(); reload() }
        .confirmationDialog(pendingDelete.map { String(localized: "Delete \($0.name.isEmpty ? String(localized: "this Mii") : $0.name)?") } ?? "",
                            isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }), titleVisibility: .visible, presenting: pendingDelete) { e in
            Button("Delete", role: .destructive) { delete(e) }
            Button("Cancel", role: .cancel) {}
        } message: { _ in Text("It disappears from every game. A copy of the database is kept as MiiDatabase.dat.bak.") }
        .confirmationDialog("Replace the Mii database?", isPresented: Binding(get: { restoreFrom != nil }, set: { if !$0 { restoreFrom = nil } }), titleVisibility: .visible, presenting: restoreFrom) { f in
            Button("Restore", role: .destructive) { restore(f) }
            Button("Cancel", role: .cancel) {}
        } message: { f in Text("All Miis are replaced by the ones in \(f.lastPathComponent). The current database is kept as MiiDatabase.dat.bak.") }
    }

    private func reload() {
        entries = s.mii.entries()
        exists = s.mii.exists
    }

    private func delete(_ e: MiiStore.Entry) {
        do { try s.mii.delete(index: e.index); note = (String(localized: "Deleted."), .good) } catch { note = (error.localizedDescription, .bad) }
        reload()
    }

    private func backUp() {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; f.locale = Locale(identifier: "en_US_POSIX")
        let p = NSSavePanel()
        p.message = String(localized: "Save a backup of the Mii database")
        p.nameFieldStringValue = "MiiDatabase-\(f.string(from: Date())).dat"
        guard p.runModal() == .OK, let url = p.url else { return }
        do { let out = try s.mii.export(to: url); note = (String(localized: "Saved \(out.lastPathComponent)."), .good) } catch { note = (error.localizedDescription, .bad) }
    }

    private func export() {
        guard let d = pick(files: false, dirs: true, message: String(localized: "Choose a folder for MiiDatabase.dat")).first else { return }
        do { let out = try s.mii.export(to: d); note = (String(localized: "Exported to \(out.path)."), .good) } catch { note = (error.localizedDescription, .bad) }
    }

    private func chooseRestore() {
        restoreFrom = pick(files: true, dirs: false, message: String(localized: "Choose a Mii database backup")).first
    }

    private func restore(_ f: URL) {
        do { try s.mii.restore(from: f); note = (String(localized: "Restored."), .good) } catch { note = (error.localizedDescription, .bad) }
        reload()
    }

    private func openFolder() {
        let folder = s.mii.folder
        do { try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true) } catch { note = (error.localizedDescription, .bad); return }
        NSWorkspace.shared.open(folder)
    }
}
