import SwiftUI
import AppKit
import NappleStudioCore

/// The one window: a glass backdrop, a header, and the current screen. Fixed at 900 x 620.
struct RootView: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        @Bindable var m = m
        ZStack {
            Backdrop()
            VStack(spacing: 0) {
                HeaderBar()
                screen.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            if m.dropTargeted && m.phase != .building {
                DropOverlay().transition(.opacity)
            }
            if let t = m.toast {
                VStack { Spacer(); ToastView(text: t).padding(.bottom, 18) }
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                    .allowsHitTesting(false)
            }
        }
        .frame(width: 900, height: 620)
        .containerBackground(.ultraThinMaterial, for: .window)
        .preferredColorScheme(.dark)
        .background(WindowConfigurator())
        .dropDestination(for: URL.self) { urls, _ in
            Task { await m.handleDrop(urls) }
            return true
        } isTargeted: { m.dropTargeted = $0 }
        .sheet(isPresented: $m.showLegal) { LegalSheet() }
        .animation(.easeOut(duration: 0.18), value: m.screen)
        .animation(.easeOut(duration: 0.15), value: m.dropTargeted)
        .animation(.easeOut(duration: 0.2), value: m.toast)
    }

    @ViewBuilder private var screen: some View {
        switch m.screen {
        case .welcome: WelcomeView()
        case .library: LibraryView()
        case .add: AddView()
        case .plan: PlanView()
        case .building: BuildingView()
        case .done: DoneView()
        case .settings: SettingsView()
        }
    }
}

/// The app icon's red and violet, glowing in two corners of the window's own glass.
struct Backdrop: View {
    var body: some View {
        ZStack {
            RadialGradient(colors: [Palette.red.opacity(0.28), .clear], center: .topTrailing, startRadius: 0, endRadius: 460)
            RadialGradient(colors: [Palette.violet.opacity(0.22), .clear], center: .bottomLeading, startRadius: 0, endRadius: 460)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// Title bar row: back arrow beside the traffic lights, the app name in the middle, Settings on the right.
struct HeaderBar: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        ZStack {
            HStack(spacing: 8) {
                Color.clear.frame(width: 64, height: 1)     // room for the traffic lights
                if m.canGoBack {
                    Button { m.goBack() } label: { Image(systemName: "chevron.left") }
                        .buttonStyle(CircleIcon(size: 28))
                        .keyboardShortcut(.cancelAction)
                        .help("Back")
                        .accessibilityLabel("Back")
                }
                Spacer()
                if m.screen != .welcome {
                    Button { m.openSettings() } label: { Label("Settings", systemImage: "gearshape") }
                        .buttonStyle(SecondaryPill(height: 32, selected: m.screen == .settings))
                        .help("Settings (⌘,)")
                        .accessibilityLabel("Settings")
                }
            }
            HStack(spacing: 8) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 26, height: 26).accessibilityHidden(true)
                Text("Napple Studio").font(.system(size: 14, weight: .semibold))
            }
            .allowsHitTesting(false)
        }
        .padding(.horizontal, 18)
        .frame(height: 52)
    }
}

/// Dragging files over any screen shows this; letting go reads them.
struct DropOverlay: View {
    var body: some View {
        RoundedRectangle(cornerRadius: 26, style: .continuous)
            .strokeBorder(Palette.red, style: StrokeStyle(lineWidth: 2, dash: [8, 6]))
            .background(RoundedRectangle(cornerRadius: 26, style: .continuous).fill(.ultraThinMaterial))
            .overlay {
                VStack(spacing: 10) {
                    Image(systemName: "tray.and.arrow.down").font(.system(size: 44)).foregroundStyle(Palette.red)
                    Text("Drop to add a game").font(.system(size: 22, weight: .semibold))
                    Text(".nsp  .xci  .nsz  .xcz  or folders").font(.system(size: 13)).foregroundStyle(Palette.secondary)
                }
            }
            .padding(14)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

/// Fixes the window at its size: no zoom or full screen, draggable by its background.
struct WindowConfigurator: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        DispatchQueue.main.async { Self.configure(v.window) }
        return v
    }
    func updateNSView(_ v: NSView, context: Context) {
        DispatchQueue.main.async { Self.configure(v.window) }
    }
    static func configure(_ w: NSWindow?) {
        guard let w else { return }
        w.standardWindowButton(.zoomButton)?.isEnabled = false
        w.isMovableByWindowBackground = true
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
    }
}

struct LegalSheet: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "lock.shield").font(.system(size: 34)).foregroundStyle(Palette.red)
            Text("Personal use only").font(.system(size: 20, weight: .semibold, design: .serif))
            Text("The app you build contains content derived from your own game files. It is for your personal use only. Do not share or distribute it.")
                .multilineTextAlignment(.center).font(.system(size: 14)).foregroundStyle(Palette.secondary)
            Button("I understand") { m.legalAck = true; m.showLegal = false; m.startBuild() }
                .buttonStyle(PrimaryPill(height: 40)).keyboardShortcut(.defaultAction)
            Button("Cancel") { m.showLegal = false }.buttonStyle(.plain).foregroundStyle(Palette.secondary)
        }
        .padding(28).frame(width: 380)
    }
}
