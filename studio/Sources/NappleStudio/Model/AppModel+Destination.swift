import SwiftUI
import NappleStudioCore

extension AppModel {
    /// The two choices in the Add panel: this Mac, or an iOS device (iPhone or iPad is told apart from the device itself).
    func chooseDestination(ios: Bool) {
        guard ios != destination.isIOS else { return }
        guard ios else { destination = .mac; return }
        let preferred = devices.first { $0.id == iosDeviceId } ?? devices.first { $0.connected && $0.supported } ?? devices.first
        destination = preferred.map { .device($0.id) } ?? .ipaIPhone
        Task { await refreshIOS() }
    }

    /// What the "Install to" menu offers: every device Studio sees, then the two .ipa exports.
    struct InstallTarget: Identifiable, Hashable {
        var destination: Destination
        var title: String
        var detail: String?
        var id: String { destination.key }
    }

    var installTargets: [InstallTarget] {
        var t = devices.map { InstallTarget(destination: .device($0.id), title: $0.name, detail: deviceStateText($0)) }
        // a remembered device that is not listed right now stays selectable, so the menu never shows a blank
        if case .device(let id) = destination, !devices.contains(where: { $0.id == id }) {
            t.insert(InstallTarget(destination: destination, title: String(localized: "Device"), detail: String(localized: "Not connected")), at: 0)
        }
        t.append(InstallTarget(destination: .ipaIPhone, title: String(localized: "Export .ipa for iPhone"), detail: nil))
        t.append(InstallTarget(destination: .ipaIPad, title: String(localized: "Export .ipa for iPad"), detail: nil))
        return t
    }

    func deviceStateText(_ d: IOSDevice) -> String {
        if !d.connected { return String(localized: "Not connected") }
        if !d.supported { return String(localized: "Needs iOS 26") }
        if !d.developerMode { return String(localized: "Developer Mode off") }
        return d.simulator ? String(localized: "Booted") : String(localized: "Ready")
    }

    /// The Build button's label.
    var buildButtonTitle: String {
        switch destination {
        case .mac: String(localized: "Build for Mac")
        case .device: String(localized: "Build and Install")
        case .ipaIPhone, .ipaIPad: String(localized: "Build .ipa")
        }
    }
}
