import SwiftUI
import AppKit
import NappleStudioCore

struct GeneralSettingsTab: View {
    @Environment(AppModel.self) private var m
    @Environment(\.openWindow) private var openWindow
    /// Shared with the main window (same key).
    @AppStorage(AppModel.playAfterBuildKey) private var playAfterBuild = false

    var body: some View {
        @Bindable var m = m
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
                Picker("Show games as", selection: $m.viewMode) {
                    ForEach(LibraryViewMode.allCases) { Label($0.label, systemImage: $0.symbol).tag($0) }
                }
                .pickerStyle(.segmented).accessibilityIdentifier("settings.general.viewMode")
                Picker("Sort by", selection: $m.sort) {
                    ForEach(LibrarySort.allCases) { Text(LocalizedStringKey($0.label)).tag($0) }
                }
                .accessibilityIdentifier("settings.general.sort")
            } header: { SettingsHeader("Library") }

            Section {
                Toggle("Play the game when the build finishes (Mac)", isOn: $playAfterBuild).accessibilityIdentifier("settings.general.playAfterBuild")
            } header: { SettingsHeader("Builds") }

            Section {
                Text("Personal use only. Built apps contain content derived from your own game files. Keep them for yourself and never share them, your keys or your firmware.")
                    .font(.callout).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                    .accessibilityIdentifier("settings.general.legal")
            } header: { SettingsHeader("Legal") }

            Section {
                LabeledContent("Napple Studio") {
                    Text(AppModel.versionString).foregroundStyle(.secondary).textSelection(.enabled)
                }
                HStack {
                    Button("About Napple Studio") { openWindow(id: "about") }
                    Button("Show Welcome Guide Again") { m.onboardingStep = 0; m.onboardingDone = false; dismissSettingsWindow() }
                        .accessibilityIdentifier("settings.general.welcome")
                }
                .buttonStyle(.glass)
            } header: { SettingsHeader("About") } footer: {
                SettingsFootnote("Napple Studio is a beta. Everything runs on this Mac: there is no account, no server and no data sent anywhere.")
            }
        }
    }

    private func dismissSettingsWindow() { NSApp.keyWindow?.close(); m.window?.makeKeyAndOrderFront(nil) }

    private func chooseOutput() {
        if let d = pick(files: false, dirs: true, message: String(localized: "Choose where built apps go")).first { m.outputDir = d }
    }
}
