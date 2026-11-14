import SwiftUI
import GameController

/// Game controllers the Mac sees right now. Studio only shows them; mapping, rumble and player slots are set in the game's own overlay (F1),
/// because each game can have its own.
@MainActor
@Observable
final class ControllerList {
    struct Pad: Identifiable, Equatable {
        var id: ObjectIdentifier
        var name: String
        var kind: String
        var battery: String?
    }
    var pads: [Pad] = []
    var tokens: [NSObjectProtocol] = []

    func start() {
        reload()
        let c = NotificationCenter.default
        tokens = [.GCControllerDidConnect, .GCControllerDidDisconnect].map { n in
            c.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in MainActor.assumeIsolated { self?.reload() } }
        }
        GCController.startWirelessControllerDiscovery {}
    }
    func stop() {
        tokens.forEach { NotificationCenter.default.removeObserver($0) }; tokens = []
        GCController.stopWirelessControllerDiscovery()
    }

    func reload() {
        pads = GCController.controllers().map { c in
            let kind: String
            if c.extendedGamepad != nil { kind = c.productCategory.isEmpty ? String(localized: "Game controller") : c.productCategory }
            else if c.microGamepad != nil { kind = String(localized: "Remote") } else { kind = String(localized: "Controller") }
            let battery = c.battery.map { b in "\(Int((b.batteryLevel * 100).rounded())) %" + (b.batteryState == .charging ? " · " + String(localized: "charging") : "") }
            return Pad(id: ObjectIdentifier(c), name: c.vendorName ?? String(localized: "Controller"), kind: kind, battery: battery)
        }
    }
}

struct ControllersSettingsTab: View {
    @State private var list = ControllerList()

    var body: some View {
        SettingsPage {
            Section {
                if list.pads.isEmpty {
                    ContentUnavailableView {
                        Label("No controller connected", systemImage: "gamecontroller")
                    } description: {
                        Text("Connect a Joy-Con, Pro Controller, DualSense or Xbox controller over Bluetooth or USB. It shows up here by itself.")
                    }
                    .frame(maxWidth: .infinity).accessibilityIdentifier("settings.controllers.empty")
                } else {
                    ForEach(list.pads) { pad in
                        HStack(spacing: 12) {
                            Image(systemName: "gamecontroller.fill").font(.title2).foregroundStyle(.tint).frame(width: 30)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(pad.name).fontWeight(.medium)
                                Text(pad.kind).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            if let b = pad.battery { Label(b, systemImage: "battery.75percent").font(.caption).foregroundStyle(.secondary).labelStyle(.titleAndIcon) }
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
            } header: {
                SettingsHeader("Connected controllers")
            } footer: {
                SettingsFootnote("Up to eight players. Games see every controller connected when they start; plug in more at any time.")
            }
            Section {
                Label("Players, button mapping and motion: press F1 in a running game.", systemImage: "person.2")
                Label("Keyboard and mouse work as a controller and as a touch screen.", systemImage: "cursorarrow.motionlines")
            } header: {
                SettingsHeader("Set up in the game")
            } footer: {
                SettingsFootnote("Each game keeps its own mapping, so a change for one game never affects another.")
            }
        }
        .onAppear { list.start() }
        .onDisappear { list.stop() }
    }
}
