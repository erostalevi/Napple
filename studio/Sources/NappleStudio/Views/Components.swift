import SwiftUI
import AppKit
import NappleStudioCore

/// A rounded glass card used for grouped content.
struct GlassCard<Content: View>: View {
    var title: LocalizedStringKey?
    var symbol: String?
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let title {
                Label { Text(title).font(.headline) } icon: { if let symbol { Image(systemName: symbol).foregroundStyle(.tint) } }
            }
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular, in: .rect(cornerRadius: 18, style: .continuous))
    }
}

enum Tone { case info, good, warn, bad }

struct Banner: View {
    var text: String
    var tone: Tone = .info
    var body: some View {
        Label { Text(text).fixedSize(horizontal: false, vertical: true).textSelection(.enabled) } icon: { Image(systemName: symbol).foregroundStyle(color) }
            .font(.callout)
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(color.opacity(0.12), in: .rect(cornerRadius: 12, style: .continuous))
            .accessibilityElement(children: .combine)
    }
    private var symbol: String {
        switch tone { case .info: "info.circle"; case .good: "checkmark.circle"; case .warn: "exclamationmark.triangle"; case .bad: "xmark.octagon" }
    }
    private var color: Color {
        switch tone { case .info: .accentColor; case .good: .green; case .warn: .orange; case .bad: .red }
    }
}

struct StatusDot: View {
    var color: Color
    var body: some View { Circle().fill(color).frame(width: 8, height: 8).accessibilityHidden(true) }
}

extension CompatStatus {
    var color: Color {
        switch self { case .perfect, .playable: .green; case .partial: .yellow; case .menus, .boots: .orange; case .nothing: .red; case .unknown: .secondary }
    }
}

/// Keys and firmware form, shared by setup and Settings.
struct KeysetForm: View {
    @Environment(AppModel.self) private var m
    let replaceName: String?
    var saveLabel: LocalizedStringKey = "Save"
    var done: () -> Void
    @State private var name = "Default"
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var error: String?
    @State private var busy = false
    @State private var scanNote: String?
    private var replacing: Bool { replaceName != nil }

    var body: some View {
        Form {
            if !replacing {
                LabeledContent("Set name") { TextField("Default", text: $name).labelsHidden().frame(width: 180) }
                LabeledContent("Folder") {
                    Button("Choose Folder…") { chooseFolder() }
                    Text("Napple finds prod.keys, title.keys and the firmware inside it.").font(.caption).foregroundStyle(.secondary)
                }
                if let scanNote { Text(scanNote).font(.caption).foregroundStyle(keys == nil ? .orange : .secondary) }
            }
            fileRow(replacing ? "prod.keys (optional)" : "prod.keys", keys) { keys = pick(files: true, dirs: false, message: "Choose prod.keys").first }
            fileRow("title.keys (optional)", titleKeys) { titleKeys = pick(files: true, dirs: false, message: "Choose title.keys").first }
            fileRow("Firmware (.zip or folder)", firmware) { firmware = pick(files: true, dirs: true, message: "Choose the firmware zip or folder").first }
            if let error { Banner(text: error, tone: .bad) }
            HStack {
                Text("Files are copied to Napple's private folder (owner-only). Key values are never shown or logged.").font(.caption).foregroundStyle(.tertiary)
                Spacer()
                Button(busy ? "Copying…" : saveLabel) { save() }
                    .buttonStyle(.glassProminent)
                    .disabled(busy || (keys == nil && titleKeys == nil && firmware == nil) || (!replacing && keys == nil))
                    .keyboardShortcut(.defaultAction)
            }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
    }

    private func fileRow(_ label: LocalizedStringKey, _ url: URL?, choose: @escaping () -> Void) -> some View {
        LabeledContent(label) {
            HStack {
                Text(url?.lastPathComponent ?? String(localized: "Not chosen")).foregroundStyle(url == nil ? .secondary : .primary).lineLimit(1).truncationMode(.middle)
                Spacer()
                Button("Choose…") { choose() }
            }
        }
    }

    private func chooseFolder() {
        guard let f = pick(files: false, dirs: true, message: String(localized: "Choose the folder with your keys and firmware")).first else { return }
        let s = KeyFinder.scan(f)
        keys = s.keys ?? keys; titleKeys = s.titleKeys ?? titleKeys; firmware = s.firmware ?? firmware
        var found: [String] = []
        if s.keys != nil { found.append("prod.keys") }; if s.titleKeys != nil { found.append("title.keys") }; if s.firmware != nil { found.append(String(localized: "firmware")) }
        scanNote = found.isEmpty ? String(localized: "Nothing recognised in that folder. Choose each file below.") : String(localized: "Found: \(found.joined(separator: ", "))")
    }

    private func save() {
        busy = true; error = nil
        let (n, k, t, f, r) = (replaceName ?? name, keys, titleKeys, firmware, replacing)
        let mgr = m.keysets
        Task.detached {
            let res = Result { try mgr.save(name: n, keys: k, titleKeys: t, firmware: f, replace: r) }
            await MainActor.run {
                busy = false
                switch res { case .success: done(); case .failure(let e): error = e.localizedDescription }
            }
        }
    }
}
