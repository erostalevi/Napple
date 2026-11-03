import SwiftUI
import NappleStudioCore

struct AmiiboSettingsTab: View {
    @State private var s = SettingsSystemModel()

    var body: some View {
        SettingsPage {
            Section {
                SettingsPathRow(title: "Amiibo list", value: $s.config.amiiboDatabase, placeholder: "URL, .json file or folder (default: community list)", identifier: "settings.amiibo.list") { s.commit(\.amiiboDatabase) }
                SettingsPathRow(title: "Key file", value: $s.config.amiiboKey, placeholder: "key_retail.bin", identifier: "settings.amiibo.key") { s.commit(\.amiiboKey) }
                SettingsPathRow(title: "Virtual amiibo folder", value: $s.config.amiiboStore, folder: true, placeholder: "default (system/amiibo)", identifier: "settings.amiibo.store") { s.commit(\.amiiboStore) }
                if !s.config.amiiboKey.isEmpty, !FileManager.default.isReadableFile(atPath: s.config.amiiboKey) {
                    SettingsBanner(text: String(localized: "The key file cannot be read."), tone: .warn).accessibilityIdentifier("settings.amiibo.keyWarning")
                }
                if let m = s.message { SettingsBanner(text: m, tone: .bad) }
            } header: {
                SettingsHeader("Amiibo")
            } footer: {
                SettingsFootnote("The data stays on this Mac; nothing is bundled. You can also change these inside a running game: F1 > Amiibo. The environment variables NAPPLE_AMIIBO_DB, NAPPLE_AMIIBO_KEY and NAPPLE_AMIIBO_STORE override this file.")
            }
        }
        .onAppear { s.reload() }
    }
}
