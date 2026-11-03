import SwiftUI
import NappleStudioCore

struct OnlineSettingsTab: View {
    @State private var s = SettingsSystemModel()

    var body: some View {
        SettingsPage {
            Section {
                SettingsPathRow(title: "Provider module", value: $s.config.onlineProvider, placeholder: "none: offline (NullProvider)", identifier: "settings.online.provider") { s.commit(\.onlineProvider) }
                status
                if let m = s.message { SettingsBanner(text: m, tone: .bad) }
            } header: {
                SettingsHeader("Online provider")
            } footer: {
                SettingsFootnote("Napple ships no online service. A provider is a library you supply; with none (or a broken one) every game runs offline. NAPPLE_ONLINE_PROVIDER overrides this setting.")
            }
        }
        .onAppear { s.reload() }
    }

    @ViewBuilder private var status: some View {
        let p = s.config.onlineProvider
        if p.isEmpty {
            Label("Offline", systemImage: "wifi.slash").foregroundStyle(.secondary).accessibilityIdentifier("settings.online.status")
        } else if s.config.onlineProviderUsable {
            Label("Provider module found", systemImage: "checkmark.circle.fill").foregroundStyle(.green).accessibilityIdentifier("settings.online.status")
        } else {
            Label("That file cannot be read: games start offline with the NullProvider.", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange).accessibilityIdentifier("settings.online.status")
        }
    }
}
