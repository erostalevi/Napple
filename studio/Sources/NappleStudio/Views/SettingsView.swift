import SwiftUI
import NappleStudioCore

struct SettingsView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        @Bindable var m = m
        TabView(selection: $m.settingsTab) {
            Tab("General", systemImage: "gear", value: "general") { GeneralSettings() }
            Tab("Keys & Firmware", systemImage: "key", value: "keys") { KeysSettings() }
            Tab("Mii", systemImage: "person.crop.square", value: "mii") { MiiSettings() }
            Tab("Amiibo", systemImage: "wave.3.right", value: "amiibo") { AmiiboSettings() }
            Tab("Online", systemImage: "network", value: "online") { OnlineSettings() }
            Tab("Engine", systemImage: "cpu", value: "engine") { EngineSettings() }
        }
        .frame(width: 620, height: 460)
    }
}

struct GeneralSettings: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        @Bindable var m = m
        Form {
            LabeledContent("Output folder") {
                HStack {
                    Text(m.outputDir.path).lineLimit(1).truncationMode(.middle)
                    Button("Change…") { if let d = pick(files: false, dirs: true, message: String(localized: "Choose where built apps go")).first { m.outputDir = d } }
                }
            }
            if let w = m.outputWarning { Text(w).font(.caption).foregroundStyle(.orange) }
            Toggle("Install on the device after building", isOn: $m.iosInstallAfterBuild)
            Toggle("Launch after installing", isOn: $m.iosLaunchAfterInstall)
            Toggle("Legal notice acknowledged", isOn: $m.legalAck)
            LabeledContent("Logs") { Button("Show in Finder") { m.showLogsInFinder() } }
        }
        .formStyle(.grouped)
    }
}

struct KeysSettings: View {
    @Environment(AppModel.self) private var m
    @State private var adding = false
    @State private var replacing: Keyset?
    var body: some View {
        @Bindable var m = m
        Form {
            Section("Sets") {
                if m.sets.isEmpty { Text("No key set yet.").foregroundStyle(.secondary) }
                Picker("Active set", selection: $m.selectedSetName) { ForEach(m.sets) { Text($0.name).tag($0.name) } }
                ForEach(m.sets) { s in
                    LabeledContent(s.name) {
                        HStack {
                            Text(s.firmwareURL == nil ? "keys only" : "keys + firmware").font(.caption).foregroundStyle(.secondary)
                            Button("Replace…") { replacing = s }
                            Button("Delete", role: .destructive) { try? m.keysets.delete(s); m.keysChanged() }
                        }
                    }
                }
            }
            Button("Add Set…") { adding = true }
        }
        .formStyle(.grouped)
        .sheet(isPresented: $adding) { sheet(nil) }
        .sheet(item: $replacing) { sheet($0.name) }
    }
    private func sheet(_ name: String?) -> some View {
        VStack(alignment: .trailing) {
            KeysetForm(replaceName: name) { m.keysChanged(); adding = false; replacing = nil }
            Button("Cancel") { adding = false; replacing = nil }.keyboardShortcut(.cancelAction).padding([.trailing, .bottom])
        }
        .frame(width: 520)
    }
}

struct EngineSettings: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Form {
            LabeledContent("napple") { Text(m.engineStatus).textSelection(.enabled).lineLimit(2).truncationMode(.middle) }
            if let e = m.engineMessage { Text(e).font(.caption).foregroundStyle(.secondary) }
            LabeledContent("Version") { Text(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev") }
            LabeledContent("Support folder") { Text(m.keysets.root.path).textSelection(.enabled).lineLimit(1).truncationMode(.middle) }
        }
        .formStyle(.grouped)
    }
}
