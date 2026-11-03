import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// The one window: the library, and the Add / Build panel sliding in from the right.
struct RootView: View {
    @Environment(AppModel.self) private var m
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        @Bindable var m = m
        ZStack(alignment: .trailing) {
            VStack(spacing: 0) {
                HeaderBar()
                LibraryView()
            }
            .opacity(m.panelOpen ? 0.5 : 1)
            .allowsHitTesting(!m.panelOpen && !m.showOnboarding)
            .padding(.trailing, 0)

            if m.panelOpen { PanelHost().transition(.move(edge: .trailing).combined(with: .opacity)) }
            if m.dropTargeted { DropOverlay() }
            if m.showOnboarding { OnboardingDialog() } else if m.showLegal { LegalDialog() }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(WindowMaterial().ignoresSafeArea())
        .overlay(alignment: .bottom) { ToastHost() }
        .animation(.smooth(duration: 0.25), value: m.panelOpen)
        .animation(.smooth(duration: 0.2), value: m.dropTargeted)
        .animation(.smooth(duration: 0.2), value: m.toast)
        .ignoresSafeArea()
        .onDrop(of: [.fileURL], isTargeted: $m.dropTargeted) { providers in
            Task {
                var urls: [URL] = []
                for p in providers {
                    if let d = try? await p.loadItem(forTypeIdentifier: UTType.fileURL.identifier) as? Data, let u = URL(dataRepresentation: d, relativeTo: nil) { urls.append(u) }
                }
                await m.handleDrop(urls)
            }
            return true
        }
        .onChange(of: m.consoleToOpen) { _, id in
            guard let id else { return }
            openWindow(id: "console", value: id)
            m.consoleToOpen = nil
        }
        .background(WindowAccessor { w in
            m.window = w
            WindowAccessor.configure(w)
        })
    }
}

/// Title row: the name, search, Add Game, Settings. The traffic lights sit at the left.
struct HeaderBar: View {
    @Environment(AppModel.self) private var m
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        @Bindable var m = m
        HStack(spacing: 10) {
            Text("Napple Studio").font(.system(size: 15, weight: .semibold)).padding(.leading, 74)
            Spacer()
            if !m.presentLibrary.isEmpty { SearchField(text: $m.searchText) }
            Button { m.pickFilesAndAdd() } label: { Label("Add Game", systemImage: "plus").fontWeight(.semibold) }
                .buttonStyle(.glassProminent).controlSize(.large).disabled(m.sets.isEmpty)
                .help(Text("Add a game (⌘O)")).accessibilityIdentifier("header.add")
            HeaderIconButton(symbol: "gearshape", label: "Settings") { openSettings() }.accessibilityIdentifier("header.settings")
        }
        .padding(.horizontal, 16).frame(height: 52)
        .contentShape(.rect)
        .gesture(WindowDragGesture())
    }
}

struct SearchField: View {
    @Binding var text: String
    @FocusState private var focused: Bool
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary).font(.system(size: 12))
            TextField("Search games", text: $text).textFieldStyle(.plain).focused($focused).frame(width: 150)
                .accessibilityIdentifier("header.search")
            if !text.isEmpty {
                Button { text = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }.buttonStyle(.plain).accessibilityLabel(Text("Clear search"))
            }
        }
        .padding(.horizontal, 10).frame(height: 30)
        .background(Theme.surface, in: .capsule)
        .overlay(Capsule().strokeBorder(focused ? Color.accentColor : .clear, lineWidth: 2))
    }
}

/// Hands the hosting NSWindow to the model and fixes its look: translucent, one size, no zoom or full screen.
struct WindowAccessor: NSViewRepresentable {
    var onWindow: (NSWindow) -> Void
    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        DispatchQueue.main.async { if let w = v.window { onWindow(w) } }
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {}

    @MainActor static func configure(_ w: NSWindow) {
        w.styleMask.remove(.resizable)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.isOpaque = false
        w.backgroundColor = .clear
        w.isMovableByWindowBackground = false
        w.collectionBehavior.insert(.fullScreenNone)
        w.standardWindowButton(.zoomButton)?.isEnabled = false
        // The title bar is hidden and the content runs under it, so the frame IS the content: fix the whole frame, keeping the top-left corner.
        var f = w.frame
        f.origin.y += f.height - Theme.windowSize.height
        f.size = Theme.windowSize
        w.setFrame(f, display: true)
        w.minSize = Theme.windowSize
        w.maxSize = Theme.windowSize
        w.makeFirstResponder(nil)   // no field starts focused
    }
}
