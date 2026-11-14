import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// The first-run guide: keys and firmware, the engine, the first game. It replaces the library until it is finished or skipped.
struct WelcomeView: View {
    @Environment(AppModel.self) private var m

    private struct Step { var title: LocalizedStringKey; var detail: LocalizedStringKey; var symbol: String }
    private let steps = [
        Step(title: "Keys and firmware", detail: "Your own dumps, copied privately", symbol: "key.fill"),
        Step(title: "Napple engine", detail: "Turns the game into an app", symbol: "cpu"),
        Step(title: "Your first game", detail: "Drop it in and build", symbol: "gamecontroller.fill"),
    ]

    var body: some View {
        HStack(spacing: 0) {
            rail.frame(width: 300)
            Divider().overlay(Theme.hairline)
            Group {
                switch m.onboardingStep {
                case 0: KeysStep()
                case 1: EngineStep()
                default: FirstGameStep()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(Theme.Space.xxl)
            .id(m.onboardingStep)
            .transition(.asymmetric(insertion: .move(edge: .trailing).combined(with: .opacity), removal: .opacity))
        }
        .animation(.smooth(duration: 0.3), value: m.onboardingStep)
        .accessibilityIdentifier("welcome")
    }

    private var rail: some View {
        VStack(alignment: .leading, spacing: 22) {
            Spacer(minLength: 0)
            AppMark(size: 72)
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) { Text("Napple Studio").font(.system(size: 24, weight: .bold)); BetaBadge() }
                Text("Turn your own Switch games into native apps for Mac and iPhone.").foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(steps.enumerated()), id: \.offset) { i, s in
                    HStack(spacing: 12) {
                        ZStack {
                            Circle().fill(i < m.onboardingStep ? Theme.good : (i == m.onboardingStep ? Theme.brand : Theme.surfaceStrong)).frame(width: 28, height: 28)
                            if i < m.onboardingStep { Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.white) }
                            else { Image(systemName: s.symbol).font(.system(size: 12, weight: .semibold)).foregroundStyle(i == m.onboardingStep ? .white : .secondary) }
                        }
                        VStack(alignment: .leading, spacing: 1) {
                            Text(s.title).fontWeight(i == m.onboardingStep ? .semibold : .regular)
                            Text(s.detail).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 6)
                    .opacity(i > m.onboardingStep ? 0.6 : 1)
                    .accessibilityElement(children: .combine)
                    .accessibilityLabel(Text("Step \(i + 1) of 3: \(Text(s.title))"))
                    .accessibilityValue(Text(i < m.onboardingStep ? "Done" : (i == m.onboardingStep ? "Current" : "Not started")))
                }
            }
            Spacer(minLength: 0)
            Text("Everything stays on this Mac. Napple never uploads your files, keys or saves.").font(.caption).foregroundStyle(.tertiary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(Theme.Space.xl)
        .frame(maxHeight: .infinity, alignment: .leading)
        .background(.ultraThinMaterial)
    }
}

/// Title, text, content, buttons: the frame all three steps share.
private struct StepFrame<Content: View, Footer: View>: View {
    let title: LocalizedStringKey
    let text: LocalizedStringKey
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.system(size: 26, weight: .bold)).accessibilityAddTraits(.isHeader)
                Text(text).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            ScrollView { VStack(alignment: .leading, spacing: 14) { content }.frame(maxWidth: .infinity, alignment: .leading) }.scrollIndicators(.never)
            HStack(spacing: 10) { footer }
        }
        .frame(maxWidth: 520, maxHeight: .infinity, alignment: .topLeading)
    }
}

// MARK: step 1

private struct KeysStep: View {
    @Environment(AppModel.self) private var m
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var scanNote: String?
    @State private var error: String?
    @State private var busy = false

    var body: some View {
        StepFrame(title: "Add your keys and firmware", text: "Napple reads your own game files with your own keys. Add them once; they are copied into a private, owner-only folder.") {
            if let set = m.selectedSet {
                HStack(spacing: 12) {
                    Image(systemName: "checkmark.seal.fill").font(.title).foregroundStyle(Theme.good)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Keys are ready").fontWeight(.semibold)
                        Text("Key set \"\(set.name)\" is in use. You can add more in Settings > Keys.").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                }
                .padding(14).background(Theme.good.opacity(0.13), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            } else {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Choose the folder").fontWeight(.semibold)
                        Text("Napple finds prod.keys, title.keys and the firmware inside it.").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Choose Folder…") { chooseFolder() }.buttonStyle(.glassProminent).accessibilityIdentifier("onboarding.folder")
                }
                .padding(12).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
                RowGroup {
                    fileRow("prod.keys", url: keys, required: true) { keys = pick(files: true, dirs: false, message: String(localized: "Choose prod.keys")).first }
                    fileRow("title.keys", url: titleKeys, required: false) { titleKeys = pick(files: true, dirs: false, message: String(localized: "Choose title.keys")).first }
                    fileRow(String(localized: "Firmware"), url: firmware, required: false, divider: false) { firmware = pick(files: true, dirs: true, message: String(localized: "Choose the firmware zip or folder")).first }
                }
                if let scanNote { Text(scanNote).font(.caption).foregroundStyle(keys == nil ? Theme.warn : .secondary) }
                if let error { Notice(text: error, tone: .bad) }
                Text("Key values are never shown or logged. Firmware is needed to build most games; you can add it later in Settings.").font(.caption).foregroundStyle(.secondary)
            }
        } footer: {
            Spacer()
            if m.selectedSet != nil {
                Button("Continue") { m.onboardingStep = 1 }.buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("onboarding.next")
            } else {
                Button(busy ? "Copying…" : "Save and Continue") { save() }
                    .buttonStyle(.glassProminent).controlSize(.large).disabled(busy || keys == nil).keyboardShortcut(.defaultAction).accessibilityIdentifier("onboarding.save")
            }
        }
    }

    private func fileRow(_ label: String, url: URL?, required: Bool, divider: Bool = true, choose: @escaping () -> Void) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Text(verbatim: label)
                Spacer()
                if let url { Text(url.lastPathComponent).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle); Image(systemName: "checkmark").foregroundStyle(Theme.good) }
                else { Text(required ? "Required" : "Optional").foregroundStyle(.secondary) }
                Button(url == nil ? "Choose…" : "Change…", action: choose).buttonStyle(.glass).controlSize(.small)
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            if divider { Divider().overlay(Theme.hairline).padding(.leading, 12) }
        }
    }

    private func chooseFolder() {
        guard let f = pick(files: false, dirs: true, message: String(localized: "Choose the folder with your keys and firmware")).first else { return }
        let s = KeyFinder.scan(f)
        keys = s.keys ?? keys; titleKeys = s.titleKeys ?? titleKeys; firmware = s.firmware ?? firmware
        var found: [String] = []
        if s.keys != nil { found.append("prod.keys") }
        if s.titleKeys != nil { found.append("title.keys") }
        if s.firmware != nil { found.append(String(localized: "firmware")) }
        scanNote = found.isEmpty ? String(localized: "Nothing recognised in that folder. Choose each file below.") : String(localized: "Found: \(found.joined(separator: ", "))")
    }

    private func save() {
        busy = true; error = nil
        let (k, t, f) = (keys, titleKeys, firmware)
        let mgr = m.keysets
        Task.detached {
            let res = Result { try mgr.save(name: "Default", keys: k, titleKeys: t, firmware: f, replace: true) }
            await MainActor.run {
                busy = false
                switch res {
                case .success(let set): m.keysChanged(); m.selectedSetName = set.name; m.onboardingStep = 1
                case .failure(let e): error = e.localizedDescription
                }
            }
        }
    }
}

// MARK: step 2

private struct EngineStep: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        StepFrame(title: "The Napple engine", text: "The engine recompiles the game's code and packs the app. It comes with Studio; you can also install newer versions later.") {
            if let msg = m.engineMessage {
                HStack(spacing: 12) { ProgressView().controlSize(.small); Text(msg); Spacer() }
                    .padding(14).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            }
            if m.cli != nil {
                HStack(spacing: 12) {
                    Image(systemName: "checkmark.seal.fill").font(.title).foregroundStyle(Theme.good)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Engine found").fontWeight(.semibold)
                        Text(m.engineSummary ?? "").font(.caption).foregroundStyle(.secondary)
                        Text(m.engineStatus).font(.system(size: 11, design: .monospaced)).foregroundStyle(.tertiary).lineLimit(1).truncationMode(.middle)
                    }
                    Spacer()
                }
                .padding(14).background(Theme.good.opacity(0.13), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            } else if m.engineMessage == nil {
                Notice(text: String(localized: "No engine found. Install one from Settings > Engine, or rebuild Studio with build_app.sh. You can add games once an engine is in place."), tone: .warn)
                HStack(spacing: 10) {
                    Button("Open Engine Settings") { m.openSettings(tab: "engine") }.buttonStyle(.glass)
                    Button("Check Again") { m.refreshEngines() }.buttonStyle(.glass)
                }
            }
        } footer: {
            Button("Back") { m.onboardingStep = 0 }.controlSize(.large)
            Spacer()
            Button("Continue") { m.onboardingStep = 2 }.buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("onboarding.next")
        }
    }
}

// MARK: step 3

private struct FirstGameStep: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        StepFrame(title: "Add your first game", text: "Drop the game file here (NSP, XCI, NSZ or XCZ), or a folder that also holds its updates and DLC. Napple shows what it found before it builds.") {
            Button { m.pickFilesAndAdd() } label: {
                VStack(spacing: 10) {
                    Image(systemName: "arrow.down.to.line").font(.system(size: 36, weight: .light)).foregroundStyle(.tint)
                    Text(m.dropTargeted ? "Release to add" : "Drop a game here").font(.headline)
                    Text("or choose files").font(.callout).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity).padding(.vertical, 34).contentShape(.rect)
            }
            .buttonStyle(.plain)
            .background(m.dropTargeted ? Theme.brand.opacity(0.12) : Theme.surface, in: .rect(cornerRadius: 18, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(m.dropTargeted ? Theme.brand : .secondary.opacity(0.4), style: StrokeStyle(lineWidth: 1.5, dash: [8, 6])))
            .accessibilityLabel(Text("Choose game files")).accessibilityIdentifier("onboarding.choose")
            Text("A build takes a few minutes and needs free disk space about the size of the game. You can keep using your Mac while it runs.").font(.caption).foregroundStyle(.secondary)
        } footer: {
            Button("Back") { m.onboardingStep = 1 }.controlSize(.large)
            Spacer()
            Button("Skip for Now") { m.onboardingDone = true }.buttonStyle(.glass).controlSize(.large).accessibilityIdentifier("onboarding.skip")
        }
    }
}
