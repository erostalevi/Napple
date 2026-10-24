import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

func pick(files: Bool, dirs: Bool, multiple: Bool = false, message: String) -> [URL] {
    let p = NSOpenPanel()
    p.canChooseFiles = files; p.canChooseDirectories = dirs; p.allowsMultipleSelection = multiple
    p.message = message
    return p.runModal() == .OK ? p.urls : []
}

struct ContentView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 10) {
            if m.showSetup && m.sets.isEmpty {
                KeysetForm(title: "Welcome. Add your keys and firmware", replaceName: nil) { m.refreshSets(); m.showSetup = false }
            } else {
                MainView()
            }
            Text("Built apps contain game-derived content for your personal use only. Do not share them.")
                .font(.caption2).foregroundStyle(.secondary).multilineTextAlignment(.center)
                .accessibilityLabel("Legal notice: built apps are for your personal use only and must not be shared")
        }
        .padding(14)
        .sheet(isPresented: Bindable(m).showLegal) { LegalSheet() }
    }
}

struct MainView: View {
    @Environment(AppModel.self) private var m
    @State private var targeted = false
    var body: some View {
        @Bindable var m = m
        VStack(spacing: 10) {
            if m.plan == nil { dropZone } else { PlanView() }
            if m.phase == .planning { ProgressView("Reading packages…").controlSize(.small) }
            if let e = m.errorText {
                Label(e, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                    .accessibilityLabel("Error: \(e)")
            }
            if m.plan != nil { BuildSection() }
        }
        .dropDestination(for: URL.self) { urls, _ in
            Task { await m.add(urls: urls) }; return true
        } isTargeted: { targeted = $0 }
    }

    var dropZone: some View {
        RoundedRectangle(cornerRadius: 14).strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [7]))
            .foregroundStyle(targeted ? Color.accentColor : Color.secondary.opacity(0.6))
            .overlay {
                VStack(spacing: 6) {
                    Image(systemName: "tray.and.arrow.down").font(.system(size: 34))
                    Text("Drop your game here").font(.headline)
                    Text("Base game, updates and DLC (.nsp .xci .nsz or folders)").font(.caption).foregroundStyle(.secondary)
                    Button("Choose Files…") {
                        let u = pick(files: true, dirs: true, multiple: true, message: "Choose game files or folders")
                        if !u.isEmpty { Task { await m.add(urls: u) } }
                    }.padding(.top, 4)
                }
            }
            .frame(maxHeight: .infinity)
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Drop zone for game files")
    }
}

struct PlanView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        // the plan can be cleared (Clear, a failed re-plan) before the parent stops showing this view
        if let plan = m.plan { content(plan) }
    }
    @ViewBuilder private func content(_ plan: PlanResult) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Game").font(.headline)
                    Spacer()
                    Button("Clear", systemImage: "xmark.circle") { m.reset() }.buttonStyle(.borderless)
                        .disabled(m.phase == .building).accessibilityLabel("Clear and start over")
                }
                CompatBanner()
                if let b = plan.base {
                    row("Base game", "\(b.titleId)  \(b.versionLabel)", PlanResult.formatBytes(b.size), nil)
                } else {
                    Label("No base game found. Add the base game too.", systemImage: "exclamationmark.triangle").foregroundStyle(.orange).font(.caption)
                }
                if !plan.updates.isEmpty {
                    Text("Updates (all included can be picked at runtime)").font(.caption).foregroundStyle(.secondary)
                    ForEach(plan.updates) { u in row("Update", "\(u.versionLabel)", "+" + PlanResult.formatBytes(u.size), (u, true)) }
                }
                if !plan.dlc.isEmpty {
                    Text("DLC (toggled at runtime)").font(.caption).foregroundStyle(.secondary)
                    ForEach(plan.dlc) { d in row("DLC", d.titleId, "+" + PlanResult.formatBytes(d.size), (d, false)) }
                }
                ForEach(plan.warnings, id: \.self) { Label($0, systemImage: "info.circle").font(.caption).foregroundStyle(.secondary) }
                HStack {
                    Text("Estimated app size").font(.caption)
                    Spacer()
                    Text(PlanResult.formatBytes(plan.totalSelectedBytes)).font(.caption.bold())
                }
                .accessibilityElement(children: .combine)
            }
        }
        .frame(maxHeight: .infinity)
    }

    @ViewBuilder
    func row(_ kind: String, _ detail: String, _ size: String, _ sel: (PlanItem, Bool)?) -> some View {
        HStack {
            if let (item, isUpdate) = sel {
                Toggle(isOn: Binding(get: { item.selected }, set: { _ in m.toggle(updates: isUpdate, id: item.id) })) {
                    Text("\(kind) \(detail)").lineLimit(1)
                }
                .toggleStyle(.checkbox).disabled(m.phase == .building)
                .accessibilityLabel("Include \(kind) \(detail), adds \(size)")
            } else {
                Label("\(kind) \(detail)", systemImage: "gamecontroller").lineLimit(1)
            }
            Spacer()
            Text(size).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
        }
    }
}

struct CompatBanner: View {
    @Environment(AppModel.self) private var m
    @State private var open = false
    var body: some View {
        if m.compatLoaded {
            if let c = m.compat {
                let ok = c.status.isAtLeastPartial
                VStack(alignment: .leading, spacing: 4) {
                    Label("Compatibility: \(c.status.label)" + (ok ? "" : ". This game may not run well yet.") + (c.notes.map { " \($0)" } ?? ""),
                          systemImage: ok ? "checkmark.seal" : "exclamationmark.triangle.fill")
                        .font(.caption).foregroundStyle(ok ? .green : .orange)
                    if !c.stages.isEmpty {
                        DisclosureGroup("Per-stage status", isExpanded: $open) {
                            VStack(alignment: .leading, spacing: 2) {
                                ForEach(c.stages, id: \.name) { st in
                                    HStack(alignment: .top) {
                                        Image(systemName: icon(st.status)).foregroundStyle(color(st.status)).frame(width: 14)
                                        Text(st.name).font(.caption2.bold()).frame(width: 90, alignment: .leading)
                                        Text(st.reason ?? st.status).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
                                    }
                                    .accessibilityElement(children: .combine)
                                    .accessibilityLabel("\(st.name): \(st.status). \(st.reason ?? "")")
                                }
                            }
                        }.font(.caption)
                    }
                }
                .padding(6).frame(maxWidth: .infinity, alignment: .leading)
                .background((ok ? Color.green : Color.orange).opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
            } else {
                Label("Compatibility: not in the matrix yet (untested).", systemImage: "questionmark.circle").font(.caption).foregroundStyle(.secondary)
                    .padding(6).frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
            }
        }
    }
    func icon(_ s: String) -> String {
        switch s.lowercased() { case "pass": "checkmark.circle.fill"; case "partial": "circle.lefthalf.filled"; case "fail": "xmark.circle.fill"
        case "blocked": "minus.circle.fill"; default: "circle.dashed" }
    }
    func color(_ s: String) -> Color {
        switch s.lowercased() { case "pass": .green; case "partial": .orange; case "fail": .red; default: .secondary }
    }
}

struct BuildSection: View {
    @Environment(AppModel.self) private var m
    @State private var showLog = false
    var body: some View {
        @Bindable var m = m
        VStack(spacing: 8) {
            HStack {
                Picker("Target", selection: .constant("macOS")) {
                    Text("macOS").tag("macOS")
                    Text("iPhone (coming in V2)").tag("iPhone").disabled(true)
                    Text("iPad (coming in V2)").tag("iPad").disabled(true)
                }.pickerStyle(.menu).accessibilityLabel("Build target")
                Picker("Keys", selection: $m.selectedSetName) {
                    ForEach(m.sets) { Text($0.name).tag($0.name) }
                }.pickerStyle(.menu).accessibilityLabel("Key and firmware set")
                    .disabled(m.phase == .building)
            }
            if m.phase == .building, let p = m.progress {
                VStack(alignment: .leading, spacing: 2) {
                    ProgressView(value: p.percent, total: 100)
                    Text("\(p.stage): \(p.message)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }.accessibilityElement(children: .combine)
                Button("Cancel") { m.cancelBuild() }
            } else if m.phase == .done {
                VStack(spacing: 6) {
                    Label("App built: \(m.builtApp?.lastPathComponent ?? "")", systemImage: "checkmark.circle.fill")
                        .font(.headline).foregroundStyle(.green)
                    if let p = m.builtApp { Text(p.path).font(.caption2).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle) }
                    ForEach(m.notes, id: \.self) { Text($0).font(.caption2).foregroundStyle(.orange).lineLimit(3) }
                    HStack {
                        Button("Launch", systemImage: "play.fill") { m.launch() }.buttonStyle(.borderedProminent)
                        Button("Reveal in Finder", systemImage: "folder") { m.reveal() }
                        Button("Install to /Applications", systemImage: "square.and.arrow.down") { m.installToApplications() }
                    }.disabled(m.builtApp == nil)
                    Button("Install to device (V2)", systemImage: "iphone") {}.disabled(true).help("Device install is coming in V2")
                    if let i = m.installMessage { Text(i).font(.caption).foregroundStyle(.secondary) }
                    Button("Build another game") { m.reset() }.buttonStyle(.borderless).font(.caption)
                }
            }
            if m.phase != .building && m.phase != .done {
                Button { m.startBuild() } label: {
                    Text("Build").font(.title3.bold()).frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent).controlSize(.large)
                .disabled(m.plan?.base == nil || m.phase == .planning || m.selectedSet == nil)
                .accessibilityLabel("Build the app")
            }
            if !m.log.isEmpty {
                DisclosureGroup("Log", isExpanded: $showLog) {
                    ScrollView {
                        Text(m.log.suffix(200).joined(separator: "\n")).font(.system(.caption2, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                    }.frame(height: 80)
                }.font(.caption)
            }
        }
    }
}

struct LegalSheet: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "lock.shield").font(.largeTitle)
            Text("Personal use only").font(.headline)
            Text("The app you build contains content derived from your own game files. It is for your personal use only. Do not share or distribute it.")
                .multilineTextAlignment(.center).font(.callout)
            Button("I understand") { m.legalAck = true; m.showLegal = false; m.startBuild() }
                .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
            Button("Cancel") { m.showLegal = false }.buttonStyle(.borderless)
        }.padding(24).frame(width: 360)
    }
}

/// Used for first launch, "Add set" and "Replace" in Settings.
struct KeysetForm: View {
    @Environment(AppModel.self) private var m
    let title: String
    let replaceName: String?
    var done: () -> Void
    @State private var name = "Default"
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var error: String?
    @State private var busy = false
    @State private var scanning = false
    @State private var scanNote: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.headline)
            if replaceName == nil { TextField("Set name", text: $name).accessibilityLabel("Set name") }
            if replaceName == nil {
                HStack {
                    Button(scanning ? "Looking…" : "Choose folder…", systemImage: "folder.badge.gearshape") { chooseFolder() }
                        .buttonStyle(.borderedProminent).disabled(scanning || busy)
                        .accessibilityLabel("Choose a folder containing keys and firmware")
                    Text("Pick the folder that holds your keys and firmware; Napple finds prod.keys, title.keys and the firmware in it.")
                        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                if let scanNote { Text(scanNote).font(.caption).foregroundStyle(keys == nil ? .orange : .secondary) }
                Text("Or choose each file:").font(.caption).foregroundStyle(.secondary)
            }
            fileRow("prod.keys (required)", keys) { keys = pick(files: true, dirs: false, message: "Choose prod.keys").first }
            fileRow("title.keys (optional)", titleKeys) { titleKeys = pick(files: true, dirs: false, message: "Choose title.keys").first }
            fileRow("Firmware (.zip or folder)", firmware) { firmware = pick(files: true, dirs: true, message: "Choose the firmware zip or folder").first }
            Text("Files are copied to Napple's private app-support folder (owner-only). Key values are never shown or logged.")
                .font(.caption).foregroundStyle(.secondary)
            if let error { Text(error).font(.caption).foregroundStyle(.red).accessibilityLabel("Error: \(error)") }
            HStack {
                Spacer()
                Button(busy ? "Copying…" : "Save") { save() }
                    .buttonStyle(.borderedProminent).disabled(busy || (keys == nil && replaceName == nil))
            }
        }
    }

    func fileRow(_ label: String, _ url: URL?, choose: @escaping () -> Void) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            Text(url?.lastPathComponent ?? "None").foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            Spacer()
            Button("Choose…", action: choose).accessibilityLabel("Choose \(label)")
        }.font(.callout)
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

struct SettingsView: View {
    @Environment(AppModel.self) private var m
    @State private var adding = false
    @State private var replacing: String?
    @State private var confirmDelete: Keyset?
    var body: some View {
        Form {
            Section("Key and firmware sets") {
                if m.sets.isEmpty { Text("No sets yet.").foregroundStyle(.secondary) }
                ForEach(m.sets) { s in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(s.name)
                            Text("keys" + (s.titleKeysURL != nil ? " + title keys" : "") + (s.firmwareURL != nil ? " + firmware" : " (no firmware)"))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Replace…") { replacing = s.name }
                        Button("Delete", role: .destructive) { confirmDelete = s }
                            .accessibilityLabel("Delete set \(s.name)")
                    }
                }
                Button("Add set…", systemImage: "plus") { adding = true }
            }
            Section("Output folder") {
                HStack {
                    Text(m.outputDir.path).font(.caption).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Button("Choose…") { if let u = pick(files: false, dirs: true, message: "Choose the output folder").first { m.outputDir = u } }
                    Button("Default") { m.outputDir = AppModel.defaultOutput }
                }
                Text("Pick a folder outside iCloud-managed Desktop/Documents so the app signature stays valid.").font(.caption2).foregroundStyle(.secondary)
            }
            Section("Logs") {
                Button("Show build logs in Finder", systemImage: "doc.text.magnifyingglass") {
                    try? FileManager.default.createDirectory(at: AppModel.logsDir, withIntermediateDirectories: true)
                    NSWorkspace.shared.open(AppModel.logsDir)
                }
            }
            Section("Legal") {
                Text("Built apps are for your personal use only. Do not share them.").font(.caption)
            }
        }
        .formStyle(.grouped).frame(width: 520, height: 480)
        .sheet(isPresented: $adding) {
            KeysetForm(title: "Add a set", replaceName: nil) { adding = false; m.refreshSets() }.padding(20).frame(width: 460)
        }
        .sheet(item: Binding(get: { replacing.map { Name(id: $0) } }, set: { replacing = $0?.id })) { n in
            KeysetForm(title: "Replace parts of \"\(n.id)\"", replaceName: n.id) { replacing = nil; m.refreshSets() }.padding(20).frame(width: 460)
        }
        .confirmationDialog("Delete this set and its copied files?", isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } })) {
            Button("Delete", role: .destructive) { if let s = confirmDelete { try? m.keysets.delete(s); m.refreshSets() } }
        }
    }
    struct Name: Identifiable { let id: String }
}
