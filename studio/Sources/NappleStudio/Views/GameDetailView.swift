import SwiftUI
import AppKit
import NappleStudioCore

/// The inspector for the selected game: play, what is inside the app, mods, saves, and housekeeping.
struct GameDetailView: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    @State private var confirmTrash = false

    private var details: GameDetails? { m.selectedDetails }

    var body: some View {
        PanelFrame {
            header
        } content: {
            if let c = item.compat, let note = c.attentionNote { Notice(text: note, tone: .warn) }
            about
            content
            modsSection
            savesSection
            manageSection
        } footer: {
            Button { m.playWithDebug(item) } label: { Label("Play with Debug", systemImage: "terminal") }
                .buttonStyle(.glass).controlSize(.large).accessibilityIdentifier("detail.debug")
            Spacer(minLength: 0)
            Button { m.launch(item.url) } label: { Label("Play", systemImage: "play.fill").padding(.horizontal, 8) }
                .buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("detail.play")
        }
        .confirmationDialog("Move \(item.entry.name) to the Trash?", isPresented: $confirmTrash, titleVisibility: .visible) {
            Button("Move to Trash", role: .destructive) { m.trashFromLibrary(item) }
            Button("Cancel", role: .cancel) {}
        } message: { Text("The app is moved to the Trash. Its saves stay where they are.") }
        .accessibilityIdentifier("detail")
    }

    // MARK: header

    private var header: some View {
        HStack(spacing: 14) {
            CoverView(image: AppModel.coverImage(for: item), name: item.entry.name, seed: item.entry.titleId, showsName: false, radius: 14)
                .frame(width: 76, height: 76).shadow(color: .black.opacity(0.25), radius: 6, y: 3)
            VStack(alignment: .leading, spacing: 4) {
                Text(item.entry.name).font(.system(size: 18, weight: .semibold)).lineLimit(2)
                HStack(spacing: 6) {
                    Text(item.entry.version.isEmpty ? item.entry.titleId : "v\(item.entry.version)").foregroundStyle(.secondary)
                    if let c = item.compat { CompatBadge(entry: c) }
                }
                .font(.caption)
            }
            Spacer(minLength: 0)
        }
    }

    // MARK: sections

    private var about: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Game")
            RowGroup {
                RowLine(label: "Title ID") { Text(item.entry.titleId).font(.system(size: 12, design: .monospaced)).foregroundStyle(.secondary).textSelection(.enabled) }
                RowLine(label: "Size") { Text(item.entry.bytes.map { PlanResult.formatBytes($0) } ?? "–").foregroundStyle(.secondary).monospacedDigit() }
                RowLine(label: "Built") { Text(item.entry.builtAt, format: .dateTime.day().month(.abbreviated).year().hour().minute()).foregroundStyle(.secondary) }
                RowLine(label: "Location", divider: false) {
                    Text(abbreviated(item.entry.path)).font(.system(size: 11.5, design: .monospaced)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        .help(item.entry.path)
                    Button("Show") { m.reveal(item.url) }.buttonStyle(.glass).controlSize(.small)
                }
            }
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Update and DLC")
            RowGroup {
                RowLine(label: "Embedded versions") {
                    Text(details.map { $0.embeddedVersions.isEmpty ? "–" : $0.embeddedVersions.joined(separator: ", ") } ?? "…")
                        .foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
                }
                RowLine(label: "New update or DLC", divider: false) {
                    Button("Choose Files…") { m.rebuildWithNewFiles(item) }.buttonStyle(.glass).controlSize(.small).accessibilityIdentifier("detail.update")
                }
            }
            Text("Choose the game's files again together with the new update or DLC. Napple rebuilds the app in the same place.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var modsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Mods")
            RowGroup {
                if let mods = details?.mods, !mods.isEmpty {
                    ForEach(Array(mods.enumerated()), id: \.element.id) { i, mod in
                        RowLine(label: LocalizedStringKey(mod.id), divider: true) {
                            if !mod.version.isEmpty { Text("v\(mod.version)").foregroundStyle(.secondary) }
                        }
                    }
                } else {
                    RowLine(label: "Applied mods") { Text(details == nil ? "…" : String(localized: "None")).foregroundStyle(.secondary) }
                }
                RowLine(label: "Change mods", divider: false) {
                    Button("Rebuild with Mods…") { m.rebuildWithMods(item) }.buttonStyle(.glass).controlSize(.small).accessibilityIdentifier("detail.mods")
                }
            }
        }
    }

    private var savesSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Saves and settings")
            RowGroup {
                RowLine(label: "Saves") {
                    Text(savesText).foregroundStyle(.secondary).monospacedDigit()
                    Button("Open Folder") { m.openSaves(item) }.buttonStyle(.glass).controlSize(.small).accessibilityIdentifier("detail.saves")
                }
                RowLine(label: "Game settings", divider: false) {
                    Text(details?.hasCustomSettings == true ? String(localized: "Saved") : String(localized: "Defaults")).foregroundStyle(.secondary)
                    Button("Open Folder") { m.openGameData(item) }.buttonStyle(.glass).controlSize(.small)
                }
            }
            Text("Saves stay on this Mac, in Napple's support folder. Display, controller and audio options are in the game's overlay (F1).")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var savesText: String {
        guard let d = details else { return "…" }
        guard d.saveFolder != nil else { return String(localized: "None yet") }
        return PlanResult.formatBytes(d.saveBytes)
    }

    private var manageSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel("Manage")
            RowGroup {
                RowLine(label: "Install in Applications") { Button("Install") { m.installToApplications(URL(fileURLWithPath: item.entry.path)) }.buttonStyle(.glass).controlSize(.small) }
                RowLine(label: "Remove from library") { Button("Remove") { m.removeFromLibrary(item.id) }.buttonStyle(.glass).controlSize(.small) }
                RowLine(label: "Move app to the Trash", divider: false) {
                    Button("Move to Trash", role: .destructive) { confirmTrash = true }.buttonStyle(.glass).controlSize(.small).tint(Theme.bad)
                }
            }
        }
    }

    private func abbreviated(_ path: String) -> String {
        path.replacingOccurrences(of: FileManager.default.homeDirectoryForCurrentUser.path, with: "~")
    }
}
