import SwiftUI
import UniformTypeIdentifiers
import NappleStudioCore

struct RootView: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        @Bindable var m = m
        VStack(spacing: 0) {
            HeaderBar()
            if m.pane == .add { StepBar().padding(.top, 4).padding(.bottom, 12).transition(.opacity) }
            content
                .frame(maxWidth: 680)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Background())
        .animation(.smooth(duration: 0.25), value: m.pane)
        .toolbar(removing: .title)
        .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
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
        .sheet(isPresented: $m.showInspector) { LogSheet() }
        .sheet(isPresented: Binding(get: { m.needsSetup && m.pane == .add && m.tour == nil }, set: { _ in })) { SetupSheet() }
        .background(WindowAccessor { m.window = $0 })
    }

    @ViewBuilder private var content: some View {
        switch m.pane {
        case .library: LibraryView().transition(.opacity)
        case .add: AddGameView().transition(.opacity)
        case .devices: DevicesView().transition(.opacity)
        }
    }
}

struct Background: View {
    var body: some View {
        LinearGradient(colors: [Color.accentColor.opacity(0.10), Color.clear], startPoint: .top, endPoint: .center)
            .background(.background)
            .ignoresSafeArea()
    }
}

/// Top row: where you are, a way back, and the few things that live outside the flow.
struct HeaderBar: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        HStack(spacing: 12) {
            if m.pane == .library {
                Text("Napple Studio").font(.title2.weight(.semibold))
            } else {
                Button { m.goHome() } label: { Label("Games", systemImage: "chevron.left") }
                    .buttonStyle(.plain).foregroundStyle(.tint).font(.body.weight(.medium))
                    .disabled(m.isBuilding).accessibilityIdentifier("header.back")
                if m.pane == .devices { Text("iPhone & iPad").font(.title2.weight(.semibold)) }
            }
            Spacer()
            if m.pane == .library {
                Button { m.pickFilesAndAdd() } label: { Label("Add Game", systemImage: "plus") }
                    .buttonStyle(.glassProminent).controlSize(.large).accessibilityIdentifier("toolbar.add")
            }
            if m.pane != .devices {
                Button { m.pane = .devices } label: { Image(systemName: "iphone.gen3") }.buttonStyle(.plain).help("iPhone & iPad").accessibilityLabel("iPhone and iPad")
            }
            Button { m.showInspector = true } label: { Image(systemName: "text.alignleft") }.buttonStyle(.plain).help("Show the log (⌥⌘L)").accessibilityLabel("Log")
            SettingsLink { Image(systemName: "gearshape") }.buttonStyle(.plain).help("Settings")
            Circle().fill(m.cli == nil ? Color.orange : Color.green).frame(width: 8, height: 8)
                .help(m.cli == nil ? "Engine not found" : (m.engineMessage ?? "Engine ready")).accessibilityLabel(m.cli == nil ? "Engine not found" : "Engine ready")
        }
        .font(.title3)
        .padding(.horizontal, 24).padding(.top, 14).padding(.bottom, 8)
    }
}

/// 1 Game, 2 Options, 3 Build, 4 Done: the whole Add flow at a glance.
struct StepBar: View {
    @Environment(AppModel.self) private var m
    private static let names: [LocalizedStringKey] = ["Game", "Options", "Build", "Done"]
    private var current: Int {
        switch m.phase {
        case .idle, .planning: 0
        case .ready: 1
        case .building: 2
        case .done: 3
        case .failed: m.plan == nil ? 0 : 2
        }
    }
    var body: some View {
        HStack(spacing: 0) {
            ForEach(0..<4, id: \.self) { i in
                if i > 0 { Rectangle().fill(i <= current ? Color.accentColor : Color.secondary.opacity(0.25)).frame(height: 2).frame(maxWidth: 60) }
                VStack(spacing: 4) {
                    ZStack {
                        Circle().fill(i <= current ? Color.accentColor : Color.secondary.opacity(0.18)).frame(width: 26, height: 26)
                        if i < current { Image(systemName: "checkmark").font(.caption.weight(.bold)).foregroundStyle(.white) }
                        else { Text("\(i + 1)").font(.caption.weight(.semibold)).foregroundStyle(i == current ? Color.white : Color.secondary) }
                    }
                    Text(Self.names[i]).font(.caption).foregroundStyle(i == current ? Color.primary : Color.secondary)
                }
            }
        }
        .animation(.smooth, value: current)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Step \(current + 1) of 4")
    }
}

/// The build log, on demand.
struct LogSheet: View {
    @Environment(AppModel.self) private var m
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        VStack(spacing: 0) {
            HStack { Text("Log").font(.headline); Spacer(); Button("Done") { dismiss() }.keyboardShortcut(.defaultAction) }.padding(14)
            Divider()
            LogInspector()
        }
        .frame(width: 640, height: 460)
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
