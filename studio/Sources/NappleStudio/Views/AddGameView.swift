import SwiftUI
import NappleStudioCore

/// Add Game, one step per screen: 1 Game, 2 Options, 3 Build, 4 Done.
struct AddGameView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        ZStack {
            switch m.phase {
            case .idle: dropZone.transition(fade)
            case .planning: ProgressView("Reading your game files…").controlSize(.large).frame(maxWidth: .infinity, maxHeight: .infinity).transition(fade)
            case .ready: PlanView().transition(fade)
            case .building: BuildingView().transition(fade)
            case .done: DoneView().transition(fade)
            case .failed: FailedView().transition(fade)
            }
        }
        .animation(.smooth(duration: 0.3), value: m.phase)
    }

    private var fade: AnyTransition { .opacity.combined(with: .scale(scale: 0.985)) }

    private var dropZone: some View {
        StepScreen(title: "Choose your game", subtitle: "Drop an NSP, XCI, NSZ or XCZ file here, or a folder that holds the game with its updates and DLC.") {
            VStack(spacing: 14) {
                Image(systemName: "arrow.down.app").font(.system(size: 52, weight: .light)).foregroundStyle(.tint)
                Text("Drop files here").font(.headline)
                Button("Choose Files…") { m.pickFilesAndAdd() }.buttonStyle(.glassProminent).controlSize(.large).accessibilityIdentifier("add.choose")
            }
            .frame(maxWidth: .infinity, minHeight: 220)
            .background(RoundedRectangle(cornerRadius: 20, style: .continuous).strokeBorder(.tertiary, style: StrokeStyle(lineWidth: 1.5, dash: [8, 6])))
            if let e = m.errorText { Banner(text: e, tone: .warn) }
        } footer: { EmptyView() }
    }
}

/// Shared frame for every step: title, one line of help, content, then the buttons.
struct StepScreen<Content: View, Footer: View>: View {
    var title: LocalizedStringKey
    var subtitle: LocalizedStringKey?
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.largeTitle.weight(.bold))
                if let subtitle { Text(subtitle).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
            }
            ScrollView { VStack(alignment: .leading, spacing: 12) { content }.padding(.vertical, 2) }.scrollIndicators(.hidden)
            footer
        }
        .padding(.horizontal, 24).padding(.bottom, 20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

struct PlanView: View {
    @Environment(AppModel.self) private var m
    @State private var advanced = false
    var body: some View {
        StepScreen(title: LocalizedStringKey(m.planTitle), subtitle: nil) {
            if let b = m.plan?.base {
                HStack {
                    Text("\(b.titleId) · \(b.versionLabel) · \(PlanResult.formatBytes(b.size))").foregroundStyle(.secondary).monospacedDigit()
                    Spacer()
                    if let c = m.compat { Label(c.status.label, systemImage: "circle.fill").foregroundStyle(c.status.color).font(.callout) }
                }
            }
            if let n = m.inputNote { Banner(text: n, tone: .info) }
            if m.plan?.base == nil { Banner(text: String(localized: "No base game in these files. Drop the base game (NSP or XCI) too."), tone: .warn) }
            if let c = m.compat, let note = c.attentionNote { Banner(text: note, tone: .warn) }
            if let w = m.plan?.warnings, !w.isEmpty { ForEach(w, id: \.self) { Banner(text: $0, tone: .warn) } }

            if let p = m.plan, !p.updates.isEmpty || !p.dlc.isEmpty {
                GlassCard(title: "Updates and DLC", symbol: "shippingbox") {
                    ForEach(p.updates) { u in itemRow(u, updates: true) }
                    ForEach(p.dlc) { d in itemRow(d, updates: false) }
                    Text("\(p.includedSummary) · \(PlanResult.formatBytes(p.totalSelectedBytes)) in total").font(.caption).foregroundStyle(.secondary)
                }
            }
            DestinationCard()
            DisclosureGroup("Advanced", isExpanded: $advanced) { EnginePickerCard().padding(.top, 8) }
            if let d = m.diskWarning { Banner(text: d, tone: .bad) }
            if let o = m.outputWarning, !m.destination.isIOS { Banner(text: o, tone: .warn) }
            if let b = m.destinationBlocker { Banner(text: b, tone: .warn) }
        } footer: {
            HStack {
                Button("Start Over") { m.resetPlanState() }.controlSize(.large)
                Spacer()
                Text(m.destination.isIOS ? "" : "Saved to \(m.outputDir.lastPathComponent)").font(.caption).foregroundStyle(.tertiary).lineLimit(1)
                Button { m.startBuild() } label: { Label("Build \(m.destinationLabel)", systemImage: "hammer.fill") }
                    .buttonStyle(.glassProminent).controlSize(.large).disabled(!m.canBuild).keyboardShortcut(.defaultAction).accessibilityIdentifier("plan.build")
            }
        }
    }

    private func itemRow(_ i: PlanItem, updates: Bool) -> some View {
        Toggle(isOn: Binding(get: { i.selected }, set: { _ in m.toggle(updates: updates, id: i.id) })) {
            HStack {
                Text(updates ? "Update \(i.versionLabel)" : "DLC \(i.titleId)")
                Spacer()
                Text(PlanResult.formatBytes(i.size)).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .toggleStyle(.switch).controlSize(.small)
    }
}

struct DestinationCard: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        @Bindable var m = m
        GlassCard(title: "Destination", symbol: "arrow.right.circle") {
            Picker("Destination", selection: $m.destination) {
                Label("This Mac", systemImage: "macbook").tag(Destination.mac)
                ForEach(m.devices) { d in
                    Label(d.summary, systemImage: d.simulator ? "iphone.and.arrow.right.inward" : "iphone.gen3").tag(Destination.device(d.id))
                }
                Label("iPhone .ipa file", systemImage: "doc.zipper").tag(Destination.ipaIPhone)
                Label("iPad .ipa file", systemImage: "doc.zipper").tag(Destination.ipaIPad)
            }
            .labelsHidden().pickerStyle(.radioGroup)
            if m.destination.isIOS, !m.target.isSimulator {
                Divider()
                SigningSummary()
            } else if m.target.isSimulator {
                Divider()
                Text("Simulator build: ad-hoc signed, installed with simctl. Nothing to sign.").font(.caption).foregroundStyle(.secondary)
                Toggle("Install after building", isOn: Binding(get: { m.iosInstallAfterBuild }, set: { m.iosInstallAfterBuild = $0 })).toggleStyle(.switch).controlSize(.small)
            }
        }
    }
}

/// Compact signing state with a link to the Devices pane; the full flow lives there.
struct SigningSummary: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label(m.signingMode.label, systemImage: "signature")
                Spacer()
                Text(m.accountKind.label).foregroundStyle(m.accountKind == .paid ? .green : .secondary)
                Button("Signing…") { m.pane = .devices }
            }
            Text(m.entitlementPlan.explanation).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if case .device = m.destination {
                Toggle("Install after building", isOn: Binding(get: { m.iosInstallAfterBuild }, set: { m.iosInstallAfterBuild = $0 })).toggleStyle(.switch).controlSize(.small)
            }
        }
    }
}

struct BuildingView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        StepScreen(title: "Building \(m.planTitle)", subtitle: nil) {
            Text(m.builtSummary + " · " + m.destinationLabel).foregroundStyle(.secondary)
            GlassCard {
                HStack(alignment: .firstTextBaseline) {
                    Text(AppModel.stageName(m.progress?.stage ?? "preflight")).font(.headline).contentTransition(.opacity)
                    Spacer()
                    Text("\(Int(m.progress?.percent ?? 0))%").font(.title2.weight(.semibold)).monospacedDigit().contentTransition(.numericText(value: m.progress?.percent ?? 0))
                    if let s = m.buildStarted { Text(s, style: .timer).monospacedDigit().foregroundStyle(.secondary).frame(minWidth: 44, alignment: .trailing) }
                }
                ProgressView(value: (m.progress?.percent ?? 0) / 100)
                    .progressViewStyle(.linear)
                    .animation(.smooth(duration: 0.25), value: m.progress?.percent)
                Text(m.progress?.message ?? "").font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    .accessibilityIdentifier("build.progress")
            }
            Text("This can take a few minutes. You can keep using your Mac.").font(.caption).foregroundStyle(.tertiary)
        } footer: {
            HStack {
                Button { m.showInspector = true } label: { Label("Show Log", systemImage: "text.alignleft") }.controlSize(.large)
                Spacer()
                Button(role: .cancel) { m.cancelBuild() } label: { Label(m.cancelling ? "Cancelling…" : "Cancel Build", systemImage: "xmark.circle") }
                    .controlSize(.large).disabled(m.cancelling)
            }
        }
    }
}

struct DoneView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        StepScreen(title: "\(m.planTitle) is ready", subtitle: nil) {
            HStack(spacing: 14) {
                Image(systemName: "checkmark.seal.fill").font(.system(size: 40)).foregroundStyle(.green).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 8) {
                        if let d = m.buildDuration { Text("Built in \(Duration.seconds(d).formatted(.units(allowed: [.minutes, .seconds], width: .abbreviated)))") }
                        if let b = m.builtBytes { Text("·"); Text(PlanResult.formatBytes(b)) }
                        Text("·"); Text(m.destinationLabel)
                    }.foregroundStyle(.secondary)
                    if let p = m.builtIPA ?? m.builtApp { Text(p.path).font(.caption.monospaced()).foregroundStyle(.tertiary).textSelection(.enabled).lineLimit(2).truncationMode(.middle) }
                }
            }
            if let i = m.installMessage { Banner(text: i, tone: m.installing ? .info : (m.installFailed ? .bad : .good)) }
            if !m.notes.isEmpty {
                GlassCard(title: "Notes", symbol: "note.text") {
                    ForEach(m.notes, id: \.self) { Text($0).font(.callout).foregroundStyle(.secondary).textSelection(.enabled) }
                }
            }
        } footer: {
            HStack(spacing: 10) {
                Button("Build Another") { m.resetPlanState() }.controlSize(.large)
                Button("Games") { m.goHome() }.controlSize(.large)
                Spacer()
                Button { m.reveal(m.builtIPA ?? m.builtApp) } label: { Label("Show in Finder", systemImage: "folder") }.controlSize(.large)
                if m.target == .macos {
                    Button { m.launch(m.builtApp) } label: { Label("Play", systemImage: "play.fill") }.buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("done.play")
                } else if let dev = m.selectedDevice {
                    Button { Task { await m.installToDevice() } } label: { Label(m.installing ? "Installing…" : "Install on \(dev.name)", systemImage: "iphone.and.arrow.forward") }
                        .buttonStyle(.glassProminent).controlSize(.large).disabled(m.installing).accessibilityIdentifier("done.install")
                }
            }
        }
    }
}

struct FailedView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        StepScreen(title: m.plan == nil ? "Could not read the game" : "The build did not finish", subtitle: nil) {
            Banner(text: m.errorText ?? String(localized: "Something went wrong."), tone: .bad)
        } footer: {
            HStack {
                Button("Start Over") { m.resetPlanState() }.controlSize(.large)
                Button("Show Log") { m.showInspector = true }.controlSize(.large)
                Spacer()
                if m.plan != nil { Button("Back to Options") { m.backToPlan() }.buttonStyle(.glassProminent).controlSize(.large) }
            }
        }
    }
}

/// The right-hand inspector with the live log.
struct LogInspector: View {
    @Environment(AppModel.self) private var m
    @State private var filter = ""
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField("Filter", text: $filter).textFieldStyle(.roundedBorder)
                Button { m.showLogsInFinder() } label: { Image(systemName: "folder") }.help("Show logs in Finder")
                Button { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(m.log.joined(separator: "\n"), forType: .string) } label: { Image(systemName: "doc.on.doc") }.help("Copy log")
            }
            .padding(10)
            Divider()
            if m.log.isEmpty {
                ContentUnavailableView("No log yet", systemImage: "text.alignleft", description: Text("Build output appears here, line by line."))
                    .frame(maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    let shown = lines
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(Array(shown.enumerated()), id: \.offset) { i, l in
                                Text(l).font(.caption.monospaced()).foregroundStyle(tone(l)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).id(i)
                            }
                        }
                        .padding(.horizontal, 10).padding(.vertical, 6)
                    }
                    .onChange(of: m.log.count) { if filter.isEmpty, let last = shown.indices.last { proxy.scrollTo(last, anchor: .bottom) } }
                }
            }
        }
    }
    private var lines: [String] { filter.isEmpty ? m.log : m.log.filter { $0.localizedCaseInsensitiveContains(filter) } }
    private func tone(_ l: String) -> Color {
        if l.hasPrefix("error") || l.contains("FAILED") { return .red }
        if l.hasPrefix("warning") { return .orange }
        if l.hasPrefix("$ ") { return .accentColor }
        return .primary
    }
}

/// Which engine builds the game: Studio's own or any installed version (listed with its build date), chosen before Build.
struct EnginePickerCard: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        GlassCard(title: "Engine", symbol: "gearshape.2") {
            HStack {
                Picker("Engine", selection: Binding(get: { m.selectedEngine?.id ?? m.engines.first?.id ?? "" }, set: { m.engineChoice = $0 })) {
                    ForEach(m.engines) { Text($0.label).tag($0.id) }
                }
                .labelsHidden().frame(maxWidth: 420).accessibilityIdentifier("plan.engine")
                Button { m.refreshEngines() } label: { Image(systemName: "arrow.clockwise") }.buttonStyle(.glass).help("Refresh the list")
                Spacer()
            }
            Text(m.engines.isEmpty ? "No engine found. Rebuild Studio with build_app.sh or set NAPPLE_CLI." : "The engine's own runtime and launcher are packed into the game: pick an older engine to build with that exact version.")
                .font(.caption).foregroundStyle(.secondary)
            if let e = m.selectedEngine ?? m.engines.first { Text(e.cliURL.path).font(.caption2.monospaced()).foregroundStyle(.tertiary).lineLimit(1).truncationMode(.middle) }
        }
    }
}
