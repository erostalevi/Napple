import SwiftUI

@main
struct StudioApp: App {
    @State private var model = AppModel()
    var body: some Scene {
        Window("Napple Studio", id: "main") {
            ContentView().environment(model)
                .frame(minWidth: 520, idealWidth: 520, minHeight: 420, idealHeight: 540)
        }
        .defaultSize(width: 520, height: 540)
        .windowResizability(.contentMinSize)
        Settings { SettingsView().environment(model) }
    }
}
