import SwiftUI
import AppKit
import NappleStudioCore

/// Home: only the library. A grid of covers, edge to edge, no card chrome.
struct LibraryView: View {
    @Environment(AppModel.self) private var m
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 20, alignment: .top), count: 5)

    var body: some View {
        let shown = m.filteredLibrary
        let searching = !m.searchText.trimmingCharacters(in: .whitespaces).isEmpty
        VStack(spacing: 0) {
            if m.keysRemoved { KeysRemovedBanner().padding(.horizontal, 28).padding(.top, 4) }
            if m.presentLibrary.isEmpty && !m.isBuilding {
                EmptyLibrary()
            } else if shown.isEmpty && searching && !m.isBuilding {
                NoResults(query: m.searchText)
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 24) {
                        if m.isBuilding { BuildingTile() }
                        ForEach(shown) { item in
                            GameTile(item: item)
                        }
                        if !searching { DropTile() }
                    }
                    .padding(.horizontal, 28).padding(.top, 18).padding(.bottom, 28)
                }
                .scrollIndicators(.automatic)
            }
        }
    }
}

struct GameTile: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    @State private var hovering = false

    private var selected: Bool { m.selectedLibraryItem == item.id }
    private var subtitle: String {
        let size = item.entry.bytes.map { PlanResult.formatBytes($0) }
        let version = item.entry.version.isEmpty ? nil : "v\(item.entry.version)"
        return [version, size].compactMap { $0 }.joined(separator: " · ")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack(alignment: .bottomTrailing) {
                CoverView(image: AppModel.coverImage(for: item), name: item.entry.name, seed: item.entry.titleId)
                if selected || hovering {
                    Button { m.launch(item.url) } label: {
                        Image(systemName: "play.fill").font(.system(size: 16)).foregroundStyle(.black).frame(width: 40, height: 40)
                            .background(.white, in: .circle).shadow(color: .black.opacity(0.35), radius: 6, y: 2)
                    }
                    .buttonStyle(.plain)
                    .padding(10)
                        .transition(.opacity.combined(with: .scale(scale: 0.9)))
                        .accessibilityLabel(Text("Play \(item.entry.name)"))
                        .accessibilityIdentifier("library.play")
                }
            }
            .overlay {
                if selected {
                    RoundedRectangle(cornerRadius: Theme.tileRadius + 3, style: .continuous).strokeBorder(Color.accentColor, lineWidth: 3).padding(-5)
                }
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(item.entry.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                HStack(spacing: 6) {
                    if let c = item.compat, c.status.needsAttention {
                        StatusDot(color: c.status.color)
                        Text(c.status.label + (subtitle.isEmpty ? "" : " · ") + (item.entry.bytes.map { PlanResult.formatBytes($0) } ?? ""))
                    } else {
                        Text(subtitle.isEmpty ? item.entry.titleId : subtitle)
                    }
                }
                .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .contentShape(.rect)
        .onHover { h in withAnimation(.easeOut(duration: 0.12)) { hovering = h } }
        .onTapGesture(count: 2) { m.selectedLibraryItem = item.id; m.launch(item.url) }
        .onTapGesture { m.selectedLibraryItem = item.id }
        .contextMenu { GameMenu(item: item) }
        .focusable().focusEffectDisabled()
        .onKeyPress(.return) { m.launch(item.url); return .handled }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("\(item.entry.name), \(subtitle)"))
        .accessibilityAddTraits(.isButton)
        .accessibilityAction(named: Text("Play")) { m.launch(item.url) }
        .accessibilityIdentifier("library.card.\(item.entry.titleId)")
    }
}

/// The game being built: its cover with a progress ring. Clicking it opens the build panel.
struct BuildingTile: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack {
                CoverView(image: nil, name: m.planTitle, seed: m.plan?.base?.titleId ?? m.planTitle)
                Rectangle().fill(.black.opacity(0.55)).clipShape(.rect(cornerRadius: Theme.tileRadius, style: .continuous))
                ProgressRing(fraction: m.buildFraction).frame(width: 64, height: 64)
            }
            .aspectRatio(1, contentMode: .fit)
            .overlay {
                if m.panelOpen { RoundedRectangle(cornerRadius: Theme.tileRadius + 3, style: .continuous).strokeBorder(Color.accentColor, lineWidth: 3).padding(-5) }
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(m.planTitle).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                HStack(spacing: 4) {
                    Text("Building ·")
                    if let s = m.buildStarted { Text(s, style: .timer).monospacedDigit() }
                }
                .font(.caption).foregroundStyle(Theme.warn)
            }
        }
        .contentShape(.rect)
        .onTapGesture { m.panelOpen = true }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("\(m.planTitle), building, \(Int(m.buildFraction * 100)) percent"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("library.building")
    }
}

struct ProgressRing: View {
    var fraction: Double
    var body: some View {
        ZStack {
            Circle().stroke(.white.opacity(0.25), lineWidth: 5)
            Circle().trim(from: 0, to: max(0.02, fraction)).stroke(.white, style: StrokeStyle(lineWidth: 5, lineCap: .round)).rotationEffect(.degrees(-90))
                .animation(.smooth(duration: 0.3), value: fraction)
            Text("\(Int(fraction * 100))%").font(.system(size: 14, weight: .bold)).foregroundStyle(.white).monospacedDigit()
        }
    }
}

/// The last tile: a drop target and a shortcut to the file picker.
struct DropTile: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { m.pickFilesAndAdd() } label: {
                VStack(spacing: 8) {
                    Image(systemName: "arrow.down.to.line").font(.system(size: 24, weight: .light))
                    Text("Drop a game file").font(.caption)
                }
                .foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
                .contentShape(.rect)
            }
            .buttonStyle(.plain)
            .aspectRatio(1, contentMode: .fit)
            .overlay(RoundedRectangle(cornerRadius: Theme.tileRadius, style: .continuous).strokeBorder(.secondary.opacity(0.5), style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
            Color.clear.frame(height: 1)
        }
        .accessibilityLabel(Text("Add a game"))
        .accessibilityIdentifier("library.dropTile")
    }
}

struct GameMenu: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    var body: some View {
        Button("Play") { m.launch(item.url) }
        Button("Play with Debug") { m.playWithDebug(item) }
        Button("Show in Finder") { m.reveal(item.url) }
        Divider()
        Button("Move to Trash", role: .destructive) { m.trashFromLibrary(item) }
        Button("Remove from Library") { m.removeFromLibrary(item.id) }
    }
}

/// No games yet: one big drop area.
struct EmptyLibrary: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "arrow.down.to.line").font(.system(size: 44, weight: .ultraLight)).foregroundStyle(.tint)
            Text("Drop a game to build it").font(.system(size: 22, weight: .semibold))
            Text("NSP, XCI, NSZ or XCZ files, or a folder with the game, its updates and DLC. It becomes a native Mac app that lives here.")
                .foregroundStyle(.secondary).multilineTextAlignment(.center).frame(maxWidth: 380)
            Button("Choose Files…") { m.pickFilesAndAdd() }.buttonStyle(.glassProminent).controlSize(.large).padding(.top, 4)
                .disabled(m.sets.isEmpty).accessibilityIdentifier("library.choose")
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).strokeBorder(.secondary.opacity(0.45), style: StrokeStyle(lineWidth: 1.5, dash: [8, 6])).padding(.horizontal, 28).padding(.bottom, 28).padding(.top, 8))
    }
}

struct NoResults: View {
    let query: String
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass").font(.system(size: 36, weight: .light)).foregroundStyle(.tertiary)
            Text("No results for \"\(query)\"").font(.title3.weight(.semibold))
            Text("Check the spelling or try a shorter name.").foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity).padding(.bottom, 60)
    }
}

/// Keys were deleted after setup: building is paused, games still play.
struct KeysRemovedBanner: View {
    @Environment(\.openSettings) private var openSettings
    @Environment(AppModel.self) private var m
    var body: some View {
        HStack(spacing: 12) {
            Circle().fill(Theme.warn).frame(width: 8, height: 8)
            Text("Your keys were removed, so building is paused. Your games still play.").frame(maxWidth: .infinity, alignment: .leading)
            Button("Add Keys in Settings") { m.settingsTab = "keys"; openSettings() }.buttonStyle(.glass)
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .background(Theme.warn.opacity(0.14), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        .accessibilityElement(children: .contain)
    }
}

extension CompatStatus {
    /// Statuses worth a dot on the tile: anything not known to be fine.
    var needsAttention: Bool {
        switch self { case .perfect, .playable, .unknown: false; default: true }
    }
}
