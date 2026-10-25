import SwiftUI
import AppKit
import NappleStudioCore

/// First launch (and any time the library is empty and there are no keys): add keys and firmware once.
struct WelcomeView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ScreenTitle(title: "Welcome to Napple Studio",
                        subtitle: "One quick setup. Add your keys and firmware once, then build as many games as you like.")
            KeysetForm(replaceName: nil, saveLabel: "Save and continue") { m.keysChanged() }
                .card(radius: 28, padding: 20)
            Spacer(minLength: 0)
            LegalFooter()
        }
        .padding(.horizontal, 40).padding(.top, 14).padding(.bottom, 14)
    }
}

/// Used for first launch, "Add set" and "Replace" in Settings.
struct KeysetForm: View {
    @Environment(AppModel.self) private var m
    let replaceName: String?
    var saveLabel = "Save"
    var done: () -> Void
    @State private var name = "Default"
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var error: String?
    @State private var busy = false
    @State private var scanning = false
    @State private var scanNote: String?

    private var replacing: Bool { replaceName != nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !replacing {
                HStack(alignment: .center, spacing: 14) {
                    Button { chooseFolder() } label: { Label(scanning ? "Looking…" : "Choose folder…", systemImage: "folder") }
                        .buttonStyle(PrimaryPill(height: 40))
                        .disabled(scanning || busy)
                        .accessibilityLabel("Choose a folder containing keys and firmware")
                    Text("Pick the folder that holds your keys and firmware. Napple finds prod.keys, title.keys and the firmware in it.")
                        .font(.system(size: 12)).foregroundStyle(Palette.secondary).fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                    HStack(spacing: 8) {
                        Text("Set name").font(.system(size: 12, weight: .medium))
                        TextField("Set name", text: $name).textFieldStyle(.plain).padding(.horizontal, 10).frame(width: 130, height: 32)
                            .background(Palette.fill, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(Palette.strokeStrong))
                            .accessibilityLabel("Set name")
                    }
                }
                if let scanNote { Text(scanNote).font(.system(size: 12)).foregroundStyle(keys == nil ? Palette.amber : Palette.secondary) }
                Text("Or choose each file:").font(.system(size: 12)).foregroundStyle(Palette.tertiary)
            }
            VStack(spacing: 0) {
                fileRow(replacing ? "prod.keys (optional)" : "prod.keys (required)", keys, showDivider: true) { keys = pick(files: true, dirs: false, message: "Choose prod.keys").first }
                fileRow("title.keys (optional)", titleKeys, showDivider: true) { titleKeys = pick(files: true, dirs: false, message: "Choose title.keys").first }
                fileRow("Firmware (.zip or folder)", firmware, showDivider: false) { firmware = pick(files: true, dirs: true, message: "Choose the firmware zip or folder").first }
            }
            .background(Palette.fill, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(Palette.stroke))
            if let error { Banner(text: error, tone: .bad) }
            HStack(alignment: .center) {
                Text("Files are copied to Napple's private app-support folder (owner-only). Key values are never shown or logged.")
                    .font(.system(size: 12)).foregroundStyle(Palette.tertiary).frame(maxWidth: 440, alignment: .leading)
                Spacer()
                Button(busy ? "Copying…" : saveLabel) { save() }
                    .buttonStyle(PrimaryPill(height: 40))
                    .disabled(busy || (keys == nil && titleKeys == nil && firmware == nil) || (!replacing && keys == nil))
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    func fileRow(_ label: String, _ url: URL?, showDivider: Bool, choose: @escaping () -> Void) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(label).font(.system(size: 13, weight: .medium)).frame(width: 190, alignment: .leading)
                Text(url?.lastPathComponent ?? "None").font(.system(size: 12, design: .monospaced))
                    .foregroundStyle(url == nil ? Palette.tertiary : Palette.secondary).lineLimit(1).truncationMode(.middle)
                Spacer()
                Button("Choose…", action: choose).buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Choose \(label)")
            }
            .padding(.horizontal, 16).padding(.vertical, 9)
            if showDivider { Divider().overlay(Palette.stroke) }
        }
    }

    func chooseFolder() {
        guard let folder = pick(files: false, dirs: true, message: "Choose the folder that contains your keys and firmware").first else { return }
        scanning = true; error = nil; scanNote = nil
        Task.detached {
            let r = KeyFinder.scan(folder)
            await MainActor.run {
                scanning = false
                if let k = r.keys { keys = k }
                if let t = r.titleKeys { titleKeys = t }
                if let f = r.firmware { firmware = f }
                let found = [r.keys.map { _ in "keys" }, r.titleKeys.map { _ in "title keys" }, r.firmware.map { "firmware (\($0.lastPathComponent))" }].compactMap { $0 }
                let missing = [r.keys == nil ? "prod.keys" : nil, r.firmware == nil ? "firmware" : nil].compactMap { $0 }
                scanNote = (found.isEmpty ? "Nothing found in \(folder.lastPathComponent)." : "Found " + found.joined(separator: ", ") + ".")
                    + (missing.isEmpty ? "" : " Not found: " + missing.joined(separator: ", ") + " (choose below).")
            }
        }
    }

    func save() {
        busy = true; error = nil
        let (n, k, t, f, r) = (replaceName ?? name, keys, titleKeys, firmware, replaceName != nil)
        let mgr = m.keysets
        Task.detached {
            do {
                try mgr.save(name: n, keys: k, titleKeys: t, firmware: f, replace: r)
                await MainActor.run { busy = false; done() }
            } catch { await MainActor.run { busy = false; self.error = error.localizedDescription } }
        }
    }
}

/// Settings, inside the same window: key sets, output folder, logs, engine.
struct SettingsView: View {
    @Environment(AppModel.self) private var m
    @State private var adding = false
    @State private var replacing: String?
    @State private var confirmDelete: Keyset?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                ScreenTitle(title: "Settings", size: 30)
                keysCard
                VStack(spacing: 0) {
                    row("Output folder", m.outputDir.path, bottomBorder: true) {
                        Button("Change…") { if let u = pick(files: false, dirs: true, message: "Choose the output folder").first { m.outputDir = u } }
                            .buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Change the output folder")
                        if m.outputDir != AppModel.defaultOutput {
                            Button("Default") { m.outputDir = AppModel.defaultOutput }.buttonStyle(SecondaryPill(height: 28))
                        }
                    }
                    row("Build logs", AppModel.logsDir.path, bottomBorder: true) {
                        Button("Open") { m.showLogsInFinder() }.buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Show build logs in Finder")
                    }
                    row("Engine", m.engineStatus, bottomBorder: false) { EmptyView() }
                }
                .background(Palette.fill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Palette.stroke))
                if let w = m.outputWarning { Banner(text: w, tone: .warn) }
                LegalFooter()
            }
            .padding(.horizontal, 40).padding(.top, 10).padding(.bottom, 14)
        }
        .scrollIndicators(.hidden)
        .sheet(isPresented: $adding) {
            VStack(alignment: .leading, spacing: 12) {
                Text("Add a set").font(.system(size: 18, weight: .semibold, design: .serif))
                KeysetForm(replaceName: nil) { adding = false; m.keysChanged() }
            }.padding(24).frame(width: 640)
        }
        .sheet(item: Binding(get: { replacing.map { Name(id: $0) } }, set: { replacing = $0?.id })) { n in
            VStack(alignment: .leading, spacing: 12) {
                Text("Replace parts of \"\(n.id)\"").font(.system(size: 18, weight: .semibold, design: .serif))
                Text("Choose only what changed. Anything you leave empty stays as it is.").font(.system(size: 12)).foregroundStyle(Palette.secondary)
                KeysetForm(replaceName: n.id) { replacing = nil; m.keysChanged() }
            }.padding(24).frame(width: 640)
        }
        .confirmationDialog("Delete this set and its copied files?", isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } })) {
            Button("Delete", role: .destructive) { if let s = confirmDelete { try? m.keysets.delete(s); m.keysChanged() } }
        } message: { Text("Games already built keep working. You can add the set again later.") }
    }

    private var keysCard: some View {
        VStack(spacing: 0) {
            HStack {
                SectionLabel("Keys and firmware")
                Spacer()
                Button { adding = true } label: { Label("Add set", systemImage: "plus") }
                    .buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Add a key set")
            }
            .padding(.horizontal, 16).padding(.vertical, 10)
            Divider().overlay(Palette.stroke)
            if m.sets.isEmpty {
                Text("No sets yet. Add one to build games.").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                    .padding(16).frame(maxWidth: .infinity, alignment: .leading)
            }
            ForEach(Array(m.sets.enumerated()), id: \.element.id) { i, s in
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 8) {
                            Text(s.name).font(.system(size: 14, weight: .medium))
                            if s.name == m.selectedSet?.name {
                                Text("In use").font(.system(size: 11, weight: .semibold)).foregroundStyle(Palette.ink)
                                    .padding(.horizontal, 8).padding(.vertical, 2).background(Palette.gradient, in: Capsule())
                            }
                        }
                        Text(detail(s)).font(.system(size: 12, design: .monospaced)).foregroundStyle(Palette.tertiary)
                    }
                    Spacer()
                    if s.name != m.selectedSet?.name {
                        Button("Use") { m.selectedSetName = s.name }.buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Use set \(s.name)")
                    }
                    Button("Replace") { replacing = s.name }.buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Replace parts of \(s.name)")
                    Button("Remove") { confirmDelete = s }.buttonStyle(SecondaryPill(height: 28)).accessibilityLabel("Delete set \(s.name)")
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
                if i < m.sets.count - 1 { Divider().overlay(Palette.stroke) }
            }
        }
        .background(Palette.fill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Palette.stroke))
    }

    private func detail(_ s: Keyset) -> String {
        "prod.keys" + (s.titleKeysURL != nil ? " · title.keys" : "") + (s.firmwareURL != nil ? " · firmware" : " · no firmware")
    }

    private func row<Actions: View>(_ title: String, _ value: String, bottomBorder: Bool, @ViewBuilder actions: () -> Actions) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(title).font(.system(size: 14, weight: .medium)).frame(width: 120, alignment: .leading)
                Text(value).font(.system(size: 12, design: .monospaced)).foregroundStyle(Palette.tertiary)
                    .lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 8)
                HStack(spacing: 8) { actions() }
            }
            .padding(.horizontal, 16).padding(.vertical, 14)
            if bottomBorder { Divider().overlay(Palette.stroke) }
        }
    }

    struct Name: Identifiable { let id: String }
}
