import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// The one window: the library in the middle, and on the right an inspector that shows either the selected game or the Add / Build flow.
/// The window resizes freely (down to `Theme.minWindowSize`); the first run shows the welcome guide instead.
struct RootView: View {
    @Environment(AppModel.self) private var m
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        @Bindable var m = m
        ZStack {
            if m.showOnboarding { WelcomeView().transition(.opacity) } else { MainView().transition(.opacity) }
            if m.dropTargeted && !m.showOnboarding { DropOverlay() }
            if m.showLegal && !m.showOnboarding { LegalDialog() }
        }
        .frame(minWidth: Theme.minWindowSize.width, minHeight: Theme.minWindowSize.height)
        .background(Backdrop())
        .overlay(alignment: .bottom) { ToastHost() }
        .tint(Theme.brand)
        .animation(.smooth(duration: 0.3), value: m.showOnboarding)
        .animation(.smooth(duration: 0.2), value: m.dropTargeted)
        .animation(.smooth(duration: 0.2), value: m.toast)
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

/// Library + toolbar + inspector.
struct MainView: View {
    @Environment(AppModel.self) private var m

    private var inspectorShown: Binding<Bool> {
        Binding(get: { m.panelOpen || m.selectedItem != nil },
                set: { if !$0 { if m.panelOpen { m.closePanel() } else { m.selectedLibraryItem = nil } } })
    }

    private var subtitle: String {
        let n = m.presentLibrary.count
        let games = n == 0 ? String(localized: "No games yet") : n == 1 ? String(localized: "1 game") : String(localized: "\(n) games")
        return "\(games) · Beta"
    }

    var body: some View {
        @Bindable var m = m
        LibraryView()
            .navigationTitle("Napple Studio")
            .navigationSubtitle(subtitle)
            .searchable(text: $m.searchText, placement: .toolbar, prompt: Text("Search games"))
            .toolbar {
                ToolbarItem(placement: .navigation) { BetaBadge() }
                ToolbarItemGroup {
                    Picker("View", selection: $m.viewMode) {
                        ForEach(LibraryViewMode.allCases) { Label($0.label, systemImage: $0.symbol).tag($0) }
                    }
                    .pickerStyle(.segmented).labelStyle(.iconOnly).help(Text("Show the library as a grid or a list"))
                    .accessibilityIdentifier("toolbar.viewMode")
                    Menu {
                        Picker("Sort by", selection: $m.sort) {
                            ForEach(LibrarySort.allCases) { Label(LocalizedStringKey($0.label), systemImage: $0.symbol).tag($0) }
                        }
                    } label: { Label("Sort", systemImage: "arrow.up.arrow.down") }
                        .help(Text("Sort the library")).accessibilityIdentifier("toolbar.sort")
                }
                ToolbarSpacer(.fixed)
                ToolbarItem {
                    Button { m.pickFilesAndAdd() } label: { Label("Add Game", systemImage: "plus") }
                        .help(Text("Add a game (⌘O)")).accessibilityIdentifier("toolbar.add")
                }
                ToolbarItem {
                    Button { m.openSettings() } label: { Label("Settings", systemImage: "gearshape") }
                        .help(Text("Settings (⌘,)")).accessibilityIdentifier("toolbar.settings")
                }
                ToolbarItem {
                    Button { inspectorShown.wrappedValue.toggle() } label: { Label("Details", systemImage: "sidebar.trailing") }
                        .help(Text("Show or hide the details")).disabled(m.selectedItem == nil && !m.panelOpen)
                        .accessibilityIdentifier("toolbar.inspector")
                }
            }
            .inspector(isPresented: inspectorShown) {
                InspectorHost()
                    .inspectorColumnWidth(min: Theme.inspectorWidth.min, ideal: Theme.inspectorWidth.ideal, max: Theme.inspectorWidth.max)
            }
    }
}

/// Right-hand inspector: the Add / Build flow while it is open, otherwise the selected game.
struct InspectorHost: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Group {
            if m.panelOpen { PanelHost() }
            else if let item = m.selectedItem { GameDetailView(item: item) }
            else { Color.clear }
        }
        .animation(.smooth(duration: 0.2), value: m.panelOpen)
    }
}

/// Hands the hosting NSWindow to the model and sets its sizes. The window keeps the system look (toolbar, Liquid Glass) and its saved frame.
struct WindowAccessor: NSViewRepresentable {
    var onWindow: (NSWindow) -> Void
    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        DispatchQueue.main.async { if let w = v.window { onWindow(w) } }
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {}

    @MainActor static func configure(_ w: NSWindow) {
        w.contentMinSize = Theme.minWindowSize
        w.tabbingMode = .disallowed
        w.makeFirstResponder(nil)   // no field starts focused
    }
}
