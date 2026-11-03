import SwiftUI
import NappleStudioCore

/// Create and delete key sets. The set created last is the one in use.
struct KeysSettingsTab: View {
    @Environment(AppModel.self) private var m
    @State private var creating = false
    @State private var pendingDelete: Keyset?
    @State private var deleting: String?
    @State private var error: String?

    var body: some View {
        SettingsPage {
            Section {
                if m.sets.isEmpty {
                    ContentUnavailableView {
                        Label("No key sets yet", systemImage: "key")
                    } description: {
                        Text("A key set holds your prod.keys, title.keys and firmware. Napple needs one to read your games.")
                    } actions: {
                        Button("Create Set…") { creating = true }.buttonStyle(.glassProminent).accessibilityIdentifier("settings.keys.createEmpty")
                    }
                    .frame(maxWidth: .infinity)
                } else {
                    ForEach(m.sets) { set in row(set) }
                }
                if let error { SettingsBanner(text: error, tone: .bad) }
            } header: {
                SettingsHeader("Key sets")
            } footer: {
                SettingsFootnote("Files are copied to Napple's private folder (owner-only). Key values are never shown or logged. The set created last is the one in use.")
            }

            if !m.sets.isEmpty {
                Section {
                    Button("Create Set…") { creating = true }.buttonStyle(.glass).accessibilityIdentifier("settings.keys.create")
                }
            }
        }
        .task { m.restoreActiveSet() }
        .sheet(isPresented: $creating) { CreateKeySetSheet().environment(m) }
        .confirmationDialog(pendingDelete.map { String(localized: "Delete the key set \"\($0.name)\"?") } ?? "", isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
                            titleVisibility: .visible, presenting: pendingDelete) { set in
            Button("Delete", role: .destructive) { delete(set) }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("The copies of its keys and firmware that Napple keeps are removed. Your original files are not touched.")
        }
    }

    private func row(_ set: Keyset) -> some View {
        let active = set.name == m.selectedSetName
        return HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(set.name).fontWeight(.medium)
                    if active {
                        Text("In use").font(.caption2.weight(.semibold)).padding(.horizontal, 6).padding(.vertical, 1)
                            .background(Color.accentColor.opacity(0.18), in: .capsule).foregroundStyle(Color.accentColor)
                            .accessibilityIdentifier("settings.keys.inUse.\(set.name)")
                    }
                }
                HStack(spacing: 10) {
                    tick("prod.keys", true)
                    tick("title.keys", set.titleKeysURL != nil)
                    tick("Firmware", set.firmwareURL != nil)
                }
            }
            Spacer()
            if deleting == set.name {
                ProgressView().controlSize(.small)
            } else {
                Button("Delete", role: .destructive) { pendingDelete = set }
                    .disabled(deleting != nil).accessibilityIdentifier("settings.keys.delete.\(set.name)")
            }
        }
        .accessibilityElement(children: .contain)
    }

    private func tick(_ title: LocalizedStringKey, _ present: Bool) -> some View {
        Label { Text(title) } icon: { Image(systemName: present ? "checkmark.circle.fill" : "minus.circle") }
            .font(.caption).foregroundStyle(present ? Color.green : Color.secondary)
            .accessibilityValue(present ? Text("present") : Text("missing"))
    }

    private func delete(_ set: Keyset) {
        error = nil; deleting = set.name
        Task {
            do { try await m.deleteKeySet(set) } catch { self.error = error.localizedDescription }
            deleting = nil
        }
    }
}

/// The "Create Set…" sheet: a folder scan or each file by hand, then save.
struct CreateKeySetSheet: View {
    @Environment(AppModel.self) private var m
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var scanNote: String?
    @State private var scanning = false
    @State private var saving = false
    @State private var error: String?

    private var cleanName: String? { KeysetManager.sanitize(name) }
    private var nameProblem: String? {
        if name.trimmingCharacters(in: .whitespaces).isEmpty { return nil }
        if cleanName == nil { return String(localized: "Use letters, digits, spaces, - and _.") }
        if m.sets.contains(where: { $0.name == cleanName }) { return String(localized: "A set with this name already exists.") }
        return nil
    }
    private var canSave: Bool { cleanName != nil && nameProblem == nil && keys != nil && !saving && !scanning }

    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section {
                    LabeledContent("Name") {
                        TextField("", text: $name, prompt: Text("e.g. Switch 1")).labelsHidden().textFieldStyle(.roundedBorder).frame(width: 220)
                            .accessibilityIdentifier("settings.keys.sheet.name")
                    }
                    if let p = nameProblem { SettingsFootnote(LocalizedStringKey(p), tone: .orange) }
                } header: { SettingsHeader("New key set") }

                Section {
                    LabeledContent("Folder") {
                        HStack {
                            if scanning { ProgressView().controlSize(.small) }
                            Button("Choose Folder…") { chooseFolder() }.disabled(scanning).accessibilityIdentifier("settings.keys.sheet.folder")
                        }
                    }
                    if let scanNote { SettingsFootnote(LocalizedStringKey(scanNote), tone: keys == nil ? .orange : .secondary) }
                    fileRow("prod.keys", keys, id: "prod") { keys = pick(files: true, dirs: false, message: String(localized: "Choose prod.keys")).first }
                    fileRow("title.keys (optional)", titleKeys, id: "title") { titleKeys = pick(files: true, dirs: false, message: String(localized: "Choose title.keys")).first }
                    fileRow("Firmware (.zip or folder, optional)", firmware, id: "firmware") {
                        firmware = pick(files: true, dirs: true, message: String(localized: "Choose the firmware zip or folder")).first
                    }
                } header: { SettingsHeader("Files") } footer: {
                    SettingsFootnote("Choose the folder that holds them and Napple finds the files, or choose each file yourself.")
                }

                if let error { SettingsBanner(text: error, tone: .bad) }
            }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)

            HStack {
                if saving { ProgressView().controlSize(.small); Text("Copying…").font(.caption).foregroundStyle(.secondary) }
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction).disabled(saving).accessibilityIdentifier("settings.keys.sheet.cancel")
                Button("Save") { save() }.buttonStyle(.glassProminent).keyboardShortcut(.defaultAction).disabled(!canSave)
                    .accessibilityIdentifier("settings.keys.sheet.save")
            }
            .padding(16)
        }
        .frame(width: 540, height: 520)
        .interactiveDismissDisabled(saving)
        .onAppear { if name.isEmpty { name = suggestedName() } }
    }

    private func suggestedName() -> String {
        if m.sets.isEmpty { return "Default" }
        var i = m.sets.count + 1
        while m.sets.contains(where: { $0.name == "Set \(i)" }) { i += 1 }
        return "Set \(i)"
    }

    private func fileRow(_ label: LocalizedStringKey, _ url: URL?, id: String, choose: @escaping () -> Void) -> some View {
        LabeledContent(label) {
            HStack {
                Text(url?.lastPathComponent ?? String(localized: "Not chosen")).foregroundStyle(url == nil ? .secondary : .primary).lineLimit(1).truncationMode(.middle)
                Button("Choose…", action: choose).accessibilityIdentifier("settings.keys.sheet.\(id)")
            }
        }
    }

    private func chooseFolder() {
        guard let f = pick(files: false, dirs: true, message: String(localized: "Choose the folder with your keys and firmware")).first else { return }
        scanning = true; scanNote = nil
        Task {
            let s = await Task.detached { KeyFinder.scan(f) }.value   // up to 20 000 directory entries: not on the main thread
            keys = s.keys ?? keys; titleKeys = s.titleKeys ?? titleKeys; firmware = s.firmware ?? firmware
            var found: [String] = []
            if s.keys != nil { found.append("prod.keys") }
            if s.titleKeys != nil { found.append("title.keys") }
            if s.firmware != nil { found.append(String(localized: "firmware")) }
            scanNote = found.isEmpty ? String(localized: "Nothing recognised in that folder. Choose each file below.") : String(localized: "Found: \(found.joined(separator: ", "))")
            scanning = false
        }
    }

    private func save() {
        guard let n = cleanName else { return }
        saving = true; error = nil
        let (k, t, f, mgr) = (keys, titleKeys, firmware, m.keysets)
        Task {
            let res = await Task.detached { Result { try mgr.save(name: n, keys: k, titleKeys: t, firmware: f, replace: false) } }.value
            saving = false
            switch res {
            case .success(let set): m.keySetCreated(set); dismiss()
            case .failure(let e): error = e.localizedDescription
            }
        }
    }
}
