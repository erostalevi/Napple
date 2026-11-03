import SwiftUI
import NappleStudioCore

struct GeneralSettingsTab: View {
    @Environment(AppModel.self) private var m
    /// Shared with the main window (same key).
    @AppStorage(AppModel.playAfterBuildKey) private var playAfterBuild = false

    var body: some View {
        SettingsPage {
            Section {
                LabeledContent("Output folder") {
                    HStack(spacing: 8) {
                        Text(m.outputDir.path).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary).textSelection(.enabled)
                            .help(m.outputDir.path).accessibilityIdentifier("settings.general.outputPath")
                        Button("Choose…") { chooseOutput() }.accessibilityIdentifier("settings.general.chooseOutput")
                    }
                }
                if let w = m.outputWarning { SettingsBanner(text: w, tone: .warn) }
            } header: { SettingsHeader("Output") } footer: {
                SettingsFootnote("Built apps are written here. Napple Studio lists everything in this folder in the Library.")
            }

            Section {
                Toggle("Play the game when the build finishes (Mac)", isOn: $playAfterBuild).accessibilityIdentifier("settings.general.playAfterBuild")
            } header: { SettingsHeader("Builds") }

            Section {
                Text("Personal use only. Built apps contain content derived from your own game files. Keep them for yourself and never share them, your keys or your firmware.")
                    .font(.callout).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                    .accessibilityIdentifier("settings.general.legal")
            } header: { SettingsHeader("Legal") }
        }
    }

    private func chooseOutput() {
        if let d = pick(files: false, dirs: true, message: String(localized: "Choose where built apps go")).first { m.outputDir = d }
    }
}
