import SwiftUI
import AppKit
import NappleStudioCore

/// The one window: a single Liquid Glass panel holding a header and the current screen. Fixed at 600 x 420, no system title bar.
struct RootView: View {
    @Environment(AppModel.self) private var m

    private static let corner: CGFloat = 26

    var body: some View {
        @Bindable var m = m
        let shape = RoundedRectangle(cornerRadius: Self.corner, style: .continuous)
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
                VStack { Spacer(); ToastView(text: t).padding(.bottom, 14) }
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                    .allowsHitTesting(false)
            }
        }
        .frame(width: Metrics.width, height: Metrics.height)
        .glassEffect(.regular.tint(Palette.red.opacity(0.06)), in: shape)
        .clipShape(shape)
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

/// A tint of the app icon's red and violet inside the glass.
struct Backdrop: View {
    var body: some View {
        ZStack {
            RadialGradient(colors: [Palette.red.opacity(0.20), .clear], center: .topTrailing, startRadius: 0, endRadius: 320)
            RadialGradient(colors: [Palette.violet.opacity(0.16), .clear], center: .bottomLeading, startRadius: 0, endRadius: 320)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// Our own traffic lights (the window has no system title bar): close and minimize work, zoom is off because the window is fixed.
struct TrafficLights: View {
    @State private var hovering = false
    var body: some View {
        HStack(spacing: 8) {
            light(Color(red: 1.0, green: 0.373, blue: 0.341), "xmark", "Close") { NSApp.keyWindow?.close() }
            light(Color(red: 0.996, green: 0.737, blue: 0.180), "minus", "Minimize") { NSApp.keyWindow?.miniaturize(nil) }
            Circle().fill(Color.white.opacity(0.22)).frame(width: 12, height: 12).accessibilityHidden(true)
        }
        .onHover { hovering = $0 }
    }
    private func light(_ color: Color, _ glyph: String, _ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Circle().fill(color).frame(width: 12, height: 12)
                .overlay(Image(systemName: glyph).font(.system(size: 7, weight: .black)).foregroundStyle(Color.black.opacity(hovering ? 0.55 : 0)))
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}

/// Header row inside the panel: traffic lights and back arrow on the left, the app name in the middle, Settings on the right.
struct HeaderBar: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        ZStack {
            HStack(spacing: 10) {
                TrafficLights()
                if m.canGoBack {
                    Button { m.goBack() } label: { Image(systemName: "chevron.left") }
                        .buttonStyle(CircleIcon(size: 24))
                        .keyboardShortcut(.cancelAction)
                        .help("Back")
                        .accessibilityLabel("Back")
                }
                Spacer()
                if m.screen != .welcome {
                    Button { m.openSettings() } label: { Label("Settings", systemImage: "gearshape") }
                        .buttonStyle(SecondaryPill(height: 28, fontSize: 11, selected: m.screen == .settings))
                        .help("Settings (⌘,)")
                        .accessibilityLabel("Settings")
                }
            }
            HStack(spacing: 6) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 20, height: 20).accessibilityHidden(true)
                Text("Napple Studio").font(.system(size: 13, weight: .semibold))
            }
            .allowsHitTesting(false)
        }
        .padding(.horizontal, 16)
        .frame(height: Metrics.header)
    }
}

/// Dragging files over any screen shows this; letting go reads them.
struct DropOverlay: View {
    var body: some View {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
            .strokeBorder(Palette.red, style: StrokeStyle(lineWidth: 2, dash: [8, 6]))
            .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Color.black.opacity(0.35)))
            .overlay {
                VStack(spacing: 10) {
                    Image(systemName: "tray.and.arrow.down").font(.system(size: 35)).foregroundStyle(Palette.red)
                    Text("Drop to add a game").font(.system(size: 19, weight: .semibold))
                    Text(".nsp  .xci  .nsz  .xcz  or folders").font(.system(size: 12)).foregroundStyle(Palette.secondary)
                }
            }
            .padding(10)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

/// Strips the system window chrome: no title bar, no system traffic lights, clear background so only the glass panel shows.
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
        w.styleMask.insert([.fullSizeContentView, .closable, .miniaturizable])
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] { w.standardWindowButton(b)?.isHidden = true }
        w.isOpaque = false
        w.backgroundColor = .clear
        w.hasShadow = true
        w.isMovableByWindowBackground = true
    }
}

struct LegalSheet: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "lock.shield").font(.system(size: 27)).foregroundStyle(Palette.red)
            Text("Personal use only").font(.system(size: 17, weight: .semibold, design: .serif))
            Text("The app you build contains content derived from your own game files. It is for your personal use only. Do not share or distribute it.")
                .multilineTextAlignment(.center).font(.system(size: 13)).foregroundStyle(Palette.secondary)
            Button("I understand") { m.legalAck = true; m.showLegal = false; m.startBuild() }
                .buttonStyle(PrimaryPill(height: 40)).keyboardShortcut(.defaultAction)
            Button("Cancel") { m.showLegal = false }.buttonStyle(.plain).foregroundStyle(Palette.secondary)
        }
        .padding(28).frame(width: 380)
    }
}
