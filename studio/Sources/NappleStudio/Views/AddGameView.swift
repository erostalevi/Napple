import SwiftUI
import NappleStudioCore

/// Add Game: drop zone -> plan (base, updates, DLC, destination) -> build progress -> done.
struct AddGameView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Group {
            switch m.phase {
            case .idle: dropZone
            case .planning: ProgressView("Reading your game files…").controlSize(.large).frame(maxWidth: .infinity, maxHeight: .infinity)
            case .ready: PlanView()
            case .building: BuildingView()
            case .done: DoneView()
            case .failed: FailedView()
            }
        }
        .animation(.snappy, value: m.phase)
    }

    private var dropZone: some View {
        ContentUnavailableView {
            Label("Add a game", systemImage: "arrow.down.app")
        } description: {
            Text(m.errorText ?? String(localized: "Drop an NSP, XCI, NSZ or XCZ file here, or a folder that holds the game with its updates and DLC."))
                .foregroundStyle(m.errorText == nil ? Color.secondary : Color.orange)
        } actions: {
            Button("Choose Files…") { m.pickFilesAndAdd() }.buttonStyle(.glassProminent).accessibilityIdentifier("add.choose")
        }
    }
}

struct PlanView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        @Bindable var m = m
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(m.planTitle).font(.largeTitle.weight(.bold))
                        if let b = m.plan?.base {
                            Text("\(b.titleId) · \(b.versionLabel) · \(PlanResult.formatBytes(b.size))").font(.callout).foregroundStyle(.secondary).monospacedDigit()
                        }
                    }
                    Spacer()
                    if let c = m.compat {
                        Label(c.status.label, systemImage: "circle.fill").foregroundStyle(c.status.color).font(.callout)
                            .padding(.horizontal, 12).padding(.vertical, 6).glassEffect(.regular, in: .capsule)
                    }
                }
                if let n = m.inputNote { Banner(text: n, tone: .info) }
                if m.plan?.base == nil { Banner(text: String(localized: "No base game in these files. Drop the base game (NSP or XCI) too."), tone: .warn) }
                if let c = m.compat, let note = c.attentionNote { Banner(text: note, tone: .warn) }
                if let w = m.plan?.warnings, !w.isEmpty { ForEach(w, id: \.self) { Banner(text: $0, tone: .warn) } }

                if let p = m.plan, !p.updates.isEmpty || !p.dlc.isEmpty {
                    GlassCard(title: "Updates and DLC", symbol: "shippingbox") {
                        Text("Everything you tick is embedded in the app and can be switched at runtime.").font(.caption).foregroundStyle(.secondary)
                        ForEach(p.updates) { u in itemRow(u, updates: true) }
                        ForEach(p.dlc) { d in itemRow(d, updates: false) }
                        Text("\(p.includedSummary) · \(PlanResult.formatBytes(p.totalSelectedBytes)) in total").font(.caption).foregroundStyle(.secondary)
                    }
                }
                EnginePickerCard()
                DestinationCard()
                if let d = m.diskWarning { Banner(text: d, tone: .bad) }
                if let o = m.outputWarning, !m.destination.isIOS { Banner(text: o, tone: .warn) }
                if let b = m.destinationBlocker { Banner(text: b, tone: .warn) }
                HStack {
                    Button("Start Over") { m.resetPlanState() }
                    Spacer()
                    Text("Output: \(m.outputDir.path)").font(.caption).foregroundStyle(.tertiary).lineLimit(1).truncationMode(.middle)
                    Button { m.startBuild() } label: { Label("Build \(m.destinationLabel)", systemImage: "hammer.fill") }
                        .buttonStyle(.glassProminent).disabled(!m.canBuild).keyboardShortcut(.defaultAction).accessibilityIdentifier("plan.build")
                }
            }
            .padding(14)
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
    private static let order = ["preflight", "provision", "ingest", "plan", "rewrite", "romfs", "fonts", "bundle", "sign", "done"]
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Building \(m.planTitle)").font(.largeTitle.weight(.bold))
            Text(m.builtSummary + " · " + m.destinationLabel).foregroundStyle(.secondary)
            GlassCard {
                ProgressView(value: (m.progress?.percent ?? 0) / 100) {
                    HStack {
                        Text(AppModel.stageName(m.progress?.stage ?? "preflight")).font(.headline)
                        Spacer()
                        if let s = m.buildStarted { Text(s, style: .timer).monospacedDigit().foregroundStyle(.secondary) }
                    }
                } currentValueLabel: {
                    Text(m.progress?.message ?? "").foregroundStyle(.secondary).lineLimit(1)
                }
                .progressViewStyle(.linear).controlSize(.large)
                .accessibilityIdentifier("build.progress")
                Divider()
                stageList
            }
            HStack {
                Spacer()
                Button(role: .cancel) { m.cancelBuild() } label: { Label(m.cancelling ? "Cancelling…" : "Cancel Build", systemImage: "xmark.circle") }
                    .buttonStyle(.glass).disabled(m.cancelling)
            }
        }
        .padding(14)
        .frame(maxWidth: 720, maxHeight: .infinity, alignment: .top)
    }

    private var stageList: some View {
        let seen = Set(m.stagesSeen), current = m.progress?.stage
        return VStack(alignment: .leading, spacing: 6) {
            ForEach(Self.order, id: \.self) { s in
                HStack(spacing: 10) {
                    Image(systemName: s == current ? "circle.dotted.circle" : seen.contains(s) ? "checkmark.circle.fill" : "circle")
                        .foregroundStyle(s == current ? Color.accentColor : seen.contains(s) ? Color.green : Color.gray.opacity(0.35))
                        .symbolEffect(.pulse, isActive: s == current)
                    Text(AppModel.stageName(s)).foregroundStyle(seen.contains(s) || s == current ? Color.primary : Color.secondary.opacity(0.5))
                }
                .font(.callout)
            }
        }
    }
}

/// Plain VStack in a ScrollView on purpose: a ContentUnavailableView followed by a wrapping Banner made AppKit loop in its
/// update-constraints pass ("more Update Constraints in Window passes than there are views") and crash once the install message appeared.
struct DoneView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack(alignment: .top, spacing: 16) {
                    Image(systemName: "checkmark.seal.fill").font(.system(size: 44)).foregroundStyle(.green).accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("\(m.planTitle) is ready").font(.largeTitle.weight(.bold))
                        HStack(spacing: 8) {
                            if let d = m.buildDuration { Text("Built in \(Duration.seconds(d).formatted(.units(allowed: [.minutes, .seconds], width: .abbreviated)))") }
                            if let b = m.builtBytes { Text("·"); Text(PlanResult.formatBytes(b)) }
                            Text("·"); Text(m.destinationLabel)
                        }
                        .foregroundStyle(.secondary)
                        if let a = m.builtApp { Text(a.path).font(.caption.monospaced()).foregroundStyle(.tertiary).textSelection(.enabled).lineLimit(2).truncationMode(.middle) }
                        if let i = m.builtIPA { Text(i.path).font(.caption.monospaced()).foregroundStyle(.tertiary).textSelection(.enabled).lineLimit(2).truncationMode(.middle) }
                    }
                }
                HStack(spacing: 10) {
                    if m.target == .macos {
                        Button { m.launch(m.builtApp) } label: { Label("Play", systemImage: "play.fill") }.buttonStyle(.glassProminent).keyboardShortcut(.defaultAction).accessibilityIdentifier("done.play")
                        Button { m.reveal(m.builtApp) } label: { Label("Show in Finder", systemImage: "folder") }.buttonStyle(.glass)
                    } else {
                        if let dev = m.selectedDevice {
                            Button { Task { await m.installToDevice() } } label: { Label(m.installing ? "Installing…" : "Install on \(dev.name)", systemImage: "iphone.and.arrow.forward") }
                                .buttonStyle(.glassProminent).disabled(m.installing).accessibilityIdentifier("done.install")
                        }
                        Button { m.reveal(m.builtIPA ?? m.builtApp) } label: { Label("Show in Finder", systemImage: "folder") }.buttonStyle(.glass)
                    }
                    Button("Build Another") { m.resetPlanState() }.buttonStyle(.glass)
                }
                if let i = m.installMessage { Banner(text: i, tone: m.installing ? .info : (i.contains("failed") || i.contains("error") ? .bad : .good)) }
                if !m.notes.isEmpty {
                    GlassCard(title: "Notes", symbol: "note.text") {
                        ForEach(m.notes, id: \.self) { Text($0).font(.callout).foregroundStyle(.secondary).textSelection(.enabled) }
                    }
                }
            }
            .padding(14)
            .frame(maxWidth: 820, alignment: .leading)
        }
    }
}

struct FailedView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        ContentUnavailableView {
            Label("The build did not finish", systemImage: "xmark.octagon").foregroundStyle(.red)
        } description: {
            Text(m.errorText ?? "").textSelection(.enabled)
        } actions: {
            if m.plan != nil { Button("Back to Plan") { m.backToPlan() }.buttonStyle(.glassProminent) }
            Button("Show Log") { m.showInspector = true }
            Button("Start Over") { m.resetPlanState() }
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
                    List(Array(lines.enumerated()), id: \.offset) { i, l in
                        Text(l).font(.caption.monospaced()).foregroundStyle(tone(l)).textSelection(.enabled).listRowSeparator(.hidden).id(i)
                    }
                    .listStyle(.plain)
                    .onChange(of: m.log.count) { if filter.isEmpty, let last = lines.indices.last { proxy.scrollTo(last, anchor: .bottom) } }
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
