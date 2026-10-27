import SwiftUI
import UniformTypeIdentifiers
import NappleStudioCore

struct RootView: View {
    @Environment(AppModel.self) private var m
    @State private var columns: NavigationSplitViewVisibility = .all

    var body: some View {
        @Bindable var m = m
        NavigationSplitView(columnVisibility: $columns) {
            Sidebar()
        } detail: {
            detail
                .navigationTitle(m.pane.title)
                .inspector(isPresented: $m.showInspector) { LogInspector().inspectorColumnWidth(min: 260, ideal: 340, max: 520) }
        }
        .toolbar { MainToolbar() }
        .overlay(alignment: .bottom) { ToastHost() }
        .overlay { if m.dropTargeted { DropOverlay() } }
        .onDrop(of: [.fileURL], isTargeted: $m.dropTargeted) { providers in
            Task {
                var urls: [URL] = []
                for p in providers { if let u = try? await p.loadItem(forTypeIdentifier: UTType.fileURL.identifier) as? Data, let s = URL(dataRepresentation: u, relativeTo: nil) { urls.append(s) } }
                await m.handleDrop(urls)
            }
            return true
        }
        .sheet(isPresented: $m.showLegal) { LegalSheet() }
        .sheet(isPresented: Binding(get: { m.needsSetup && m.pane == .add && m.tour == nil }, set: { _ in })) { SetupSheet() }
        .background(WindowAccessor { m.window = $0 })
    }

    @ViewBuilder private var detail: some View {
        switch m.pane {
        case .library: LibraryView()
        case .add: AddGameView()
        case .devices: DevicesView()
        }
    }
}

struct Sidebar: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        @Bindable var m = m
        List(selection: $m.pane) {
            Section("Games") {
                row(.library, badge: m.libraryItems.isEmpty ? nil : "\(m.libraryItems.count)")
                row(.add, badge: m.isBuilding ? "●" : nil)
            }
            Section("Devices") { row(.devices, badge: nil) }
        }
        .listStyle(.sidebar)
        .navigationSplitViewColumnWidth(min: 180, ideal: 210, max: 280)
        .safeAreaInset(edge: .bottom) { EngineFooter() }
    }
    private func row(_ p: Pane, badge: String?) -> some View {
        NavigationLink(value: p) {
            Label(p.title, systemImage: p.symbol).badge(badge ?? "")
        }
        .accessibilityIdentifier("sidebar.\(p.rawValue)")
    }
}

struct EngineFooter: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let e = m.engineMessage { Text(e).font(.caption).foregroundStyle(.secondary).lineLimit(2) }
            Label(m.cli == nil ? "Engine not found" : "Engine ready", systemImage: m.cli == nil ? "exclamationmark.triangle" : "checkmark.seal")
                .font(.caption).foregroundStyle(m.cli == nil ? .orange : .secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
    }
}

struct MainToolbar: ToolbarContent {
    @Environment(AppModel.self) private var m
    var body: some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            Button { m.pickFilesAndAdd() } label: { Label("Add Game", systemImage: "plus") }
                .help("Add a game file (⌘O)").accessibilityIdentifier("toolbar.add")
            if m.isBuilding {
                Button(role: .cancel) { m.cancelBuild() } label: { Label("Cancel Build", systemImage: "xmark.circle") }
            } else if m.canBuild && m.pane == .add {
                Button { m.startBuild() } label: { Label("Build", systemImage: "hammer") }
                    .buttonStyle(.glassProminent).help("Build (⌘B)").accessibilityIdentifier("toolbar.build")
            }
        }
        ToolbarSpacer(.fixed, placement: .primaryAction)
        ToolbarItem(placement: .primaryAction) {
            Toggle(isOn: Binding(get: { m.showInspector }, set: { m.showInspector = $0 })) { Label("Log", systemImage: "sidebar.trailing") }
                .help("Show the build log (⌥⌘L)")
        }
    }
}

struct ToastHost: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        if let t = m.toast {
            Text(t).font(.callout).padding(.horizontal, 16).padding(.vertical, 10)
                .glassEffect(.regular, in: .capsule)
                .padding(.bottom, 18)
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .accessibilityAddTraits(.updatesFrequently)
                .animation(.snappy, value: m.toast)
        }
    }
}

struct DropOverlay: View {
    var body: some View {
        ContentUnavailableView("Drop to add", systemImage: "arrow.down.doc", description: Text("NSP, XCI, NSZ or XCZ files, or folders holding them."))
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(.ultraThinMaterial)
            .overlay(RoundedRectangle(cornerRadius: 24, style: .continuous).strokeBorder(.tint, style: StrokeStyle(lineWidth: 2, dash: [10, 8])).padding(16))
            .allowsHitTesting(false)
    }
}

struct LegalSheet: View {
    @Environment(AppModel.self) private var m
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("Personal use only", systemImage: "hand.raised").font(.title2.weight(.semibold))
            Text("Built apps contain content derived from your own game files. Keep them for yourself and never share them, your keys or your firmware.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("I Understand") { m.legalAck = true; dismiss(); m.startBuild() }.buttonStyle(.glassProminent).keyboardShortcut(.defaultAction)
            }
        }
        .padding(24).frame(width: 440)
    }
}

/// First-run sheet: keys and firmware, once.
struct SetupSheet: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Welcome to Napple Studio", systemImage: "sparkles").font(.title.weight(.semibold))
            Text("One quick setup. Add your keys and firmware once, then build as many games as you like.").foregroundStyle(.secondary)
            KeysetForm(replaceName: nil, saveLabel: "Save and Continue") { m.keysChanged() }
            HStack { Spacer(); Button("Later") { m.pane = .library } }
        }
        .padding(24).frame(width: 560)
    }
}

/// Hands the hosting NSWindow to the model (window restoration name, debug screenshots).
struct WindowAccessor: NSViewRepresentable {
    var onWindow: (NSWindow) -> Void
    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        DispatchQueue.main.async { if let w = v.window { onWindow(w) } }
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {}
}
