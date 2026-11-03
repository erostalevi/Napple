import SwiftUI
import NappleStudioCore

/// Loads the GitHub release list and installs one, with progress and cancellation. All disk and network work happens in `EngineStore` off the main thread.
@MainActor
@Observable
final class EngineSettingsModel {
    enum Load: Equatable { case idle, loading, loaded, failed(String) }
    var load: Load = .idle
    var releases: [EngineRelease] = []
    var installingTag: String?
    var stage: EngineInstallStage?
    var note: (text: String, tone: SettingsBanner.Tone)?
    private var installTask: Task<Void, Never>?
    private var loadTask: Task<Void, Never>?

    var latest: EngineRelease? { EngineStore.latest(in: releases) }

    func refresh(store: EngineStore) {
        loadTask?.cancel()
        load = .loading
        loadTask = Task {
            do {
                let r = try await store.fetchReleases()
                guard !Task.isCancelled else { return }
                releases = r; load = .loaded
            } catch is CancellationError {
            } catch {
                guard !Task.isCancelled else { return }
                load = .failed(error.localizedDescription)
            }
        }
    }

    func install(_ release: EngineRelease, model m: AppModel) {
        guard installingTag == nil else { return }
        installingTag = release.tag; stage = .downloading(-1); note = nil
        let store = m.engineStore, protected: Set<String> = [m.engineChoice]
        installTask = Task {
            do {
                let res = try await store.install(release, protecting: protected) { s in Task { @MainActor in self.stage = s } }
                m.refreshEngines()
                note = (String(localized: "Installed \(res.version). Choose it under Installed versions to build with it."), .good)
            } catch is CancellationError {
                note = (String(localized: "Installation cancelled."), .info)
            } catch {
                note = (error.localizedDescription, .bad)
            }
            installingTag = nil; stage = nil
        }
    }

    func cancelInstall() { installTask?.cancel() }
    func stop() { loadTask?.cancel() }
}

struct EngineSettingsTab: View {
    @Environment(AppModel.self) private var m
    @State private var model = EngineSettingsModel()
    @State private var pendingRemove: EngineBuild?
    @State private var removeError: String?
    @State private var removing = false

    var body: some View {
        SettingsPage {
            installedSection
            githubSection
        }
        .onAppear {
            m.refreshEngines()
            if model.load == .idle { model.refresh(store: m.engineStore) }
        }
        .onDisappear { model.stop() }
        .confirmationDialog(pendingRemove.map { String(localized: "Remove engine \($0.version)?") } ?? "", isPresented: Binding(get: { pendingRemove != nil }, set: { if !$0 { pendingRemove = nil } }),
                            titleVisibility: .visible, presenting: pendingRemove) { b in
            Button("Remove", role: .destructive) { remove(b) }
            Button("Cancel", role: .cancel) {}
        } message: { b in
            Text("The folder of version \(b.version) is deleted from this Mac. You can install it again from GitHub.")
        }
    }

    // MARK: installed

    private var installedSection: some View {
        let selected = m.effectiveEngineID
        return Section {
            if m.engines.isEmpty {
                ContentUnavailableView {
                    Label("No engine found", systemImage: "cpu")
                } description: {
                    Text("Studio needs the Napple engine to build games. Install a version from GitHub below, or rebuild Studio with build_app.sh.")
                }
                .frame(maxWidth: .infinity).accessibilityIdentifier("settings.engine.empty")
            }
            ForEach(m.engines) { e in
                HStack(spacing: 8) {
                    SettingsRadioRow(title: LocalizedStringKey(e.label), selected: selected == e.id, identifier: "settings.engine.pick.\(e.id)") { m.engineChoice = e.id }
                    if !e.isBundled, !e.isActive, e.id != selected {
                        Button("Remove", role: .destructive) { pendingRemove = e }
                            .disabled(removing || model.installingTag != nil).accessibilityIdentifier("settings.engine.remove.\(e.id)")
                    }
                }
            }
            if let removeError { SettingsBanner(text: removeError, tone: .bad) }
            if let msg = m.engineMessage { SettingsFootnote(LocalizedStringKey(msg)) }
        } header: {
            SettingsHeader("Installed versions")
        } footer: {
            SettingsFootnote("Studio builds with the selected engine. The one used by the Terminal's napple (marked active) and the selected one cannot be removed.")
        }
    }

    private func remove(_ b: EngineBuild) {
        removing = true; removeError = nil
        let store = m.engineStore
        Task {
            do { try await Task.detached { try store.remove(b) }.value } catch { removeError = error.localizedDescription }
            m.refreshEngines()
            removing = false
        }
    }

    // MARK: github

    private var githubSection: some View {
        Section {
            switch model.load {
            case .idle:
                HStack { ProgressView().controlSize(.small); Text("Looking for releases…").foregroundStyle(.secondary) }
            case .loading where model.releases.isEmpty:
                HStack { ProgressView().controlSize(.small); Text("Looking for releases…").foregroundStyle(.secondary) }.accessibilityIdentifier("settings.engine.loading")
            case .failed(let msg):
                SettingsBanner(text: msg, tone: .bad).accessibilityIdentifier("settings.engine.error")
                if !model.releases.isEmpty { releaseRows }
            default:
                if model.releases.isEmpty {
                    ContentUnavailableView {
                        Label("No releases", systemImage: "shippingbox")
                    } description: { Text("Nothing has been published on GitHub yet.") }
                    .frame(maxWidth: .infinity).accessibilityIdentifier("settings.engine.noReleases")
                } else { releaseRows }
            }
            if let n = model.note { SettingsBanner(text: n.text, tone: n.tone).accessibilityIdentifier("settings.engine.note") }
            HStack {
                Button { model.refresh(store: m.engineStore) } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    .buttonStyle(.glass).disabled(model.load == .loading || model.installingTag != nil).accessibilityIdentifier("settings.engine.refresh")
                if model.load == .loading { ProgressView().controlSize(.small) }
            }
        } header: {
            SettingsHeader("Available on GitHub")
        } footer: {
            SettingsFootnote("Downloads come from github.com/\(EngineStore.repo) and are checked against the release's SHA256SUMS.txt before they are installed. Installing a version does not change what the Terminal's napple uses.")
        }
    }

    @ViewBuilder private var releaseRows: some View {
        let latest = model.latest
        ForEach(model.releases) { r in releaseRow(r, isLatest: r.id == latest?.id) }
    }

    private func installed(_ r: EngineRelease) -> Bool {
        let folder = EngineStore.folderName(for: r.version)
        return m.engines.contains { !$0.isBundled && ($0.id == folder || $0.version == r.version) }
    }

    private func releaseRow(_ r: EngineRelease, isLatest: Bool) -> some View {
        let busy = model.installingTag == r.tag
        return HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(r.version).fontWeight(.medium).monospacedDigit()
                    if isLatest { badge("Latest", .accentColor) }
                    if r.prerelease { badge("Pre-release", .orange) }
                }
                Text(details(r)).font(.caption).foregroundStyle(.secondary)
                if busy, let stage = model.stage { progressView(stage) }
            }
            Spacer(minLength: 8)
            if busy {
                Button("Cancel") { model.cancelInstall() }.accessibilityIdentifier("settings.engine.cancel")
            } else if installed(r) {
                Label("Installed", systemImage: "checkmark").foregroundStyle(.secondary).font(.callout).accessibilityIdentifier("settings.engine.installed.\(r.version)")
            } else if !r.installable {
                Text("No macOS build").font(.caption).foregroundStyle(.secondary)
            } else {
                Button("Install") { model.install(r, model: m) }
                    .buttonStyle(.glass).disabled(model.installingTag != nil).accessibilityIdentifier("settings.engine.install.\(r.version)")
            }
        }
        .accessibilityElement(children: .contain)
    }

    private func badge(_ t: LocalizedStringKey, _ c: Color) -> some View {
        Text(t).font(.caption2.weight(.semibold)).padding(.horizontal, 6).padding(.vertical, 1).background(c.opacity(0.18), in: .capsule).foregroundStyle(c)
    }

    private func details(_ r: EngineRelease) -> String {
        var parts: [String] = []
        if let d = r.publishedAt { parts.append(d.formatted(date: .abbreviated, time: .omitted)) }
        if let z = r.zip { parts.append(ByteCountFormatter.string(fromByteCount: z.size, countStyle: .file)) }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder private func progressView(_ s: EngineInstallStage) -> some View {
        switch s {
        case .downloading(let f) where f >= 0:
            ProgressView(value: f) { Text("Downloading… \(Int(f * 100))%").font(.caption) }.accessibilityIdentifier("settings.engine.progress")
        case .downloading: ProgressView { Text("Downloading…").font(.caption) }.progressViewStyle(.linear)
        case .verifying: ProgressView { Text("Checking the download…").font(.caption) }.progressViewStyle(.linear)
        case .extracting: ProgressView { Text("Unpacking…").font(.caption) }.progressViewStyle(.linear)
        case .installing: ProgressView { Text("Installing…").font(.caption) }.progressViewStyle(.linear)
        }
    }
}
