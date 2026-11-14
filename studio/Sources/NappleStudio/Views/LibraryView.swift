import SwiftUI
import AppKit
import NappleStudioCore

/// Home: the games, as a grid of covers or a list. Click selects (the inspector shows the game), double-click or Return plays.
struct LibraryView: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        let shown = m.filteredLibrary
        VStack(spacing: 0) {
            if m.keysRemoved { KeysRemovedBanner().padding(.horizontal, Theme.gridPadding).padding(.top, Theme.Space.s) }
            if m.presentLibrary.isEmpty && !m.isBuilding {
                EmptyLibrary()
            } else if shown.isEmpty && m.isSearching && !m.isBuilding {
                NoResults(query: m.searchText)
            } else {
                switch m.viewMode {
                case .grid: GridLibrary(items: shown)
                case .list: ListLibrary(items: shown)
                }
            }
        }
    }
}

// MARK: - grid

struct GridLibrary: View {
    @Environment(AppModel.self) private var m
    let items: [LibraryItem]
    @FocusState private var focused: Bool

    var body: some View {
        GeometryReader { geo in
            let usable = max(geo.size.width - Theme.gridPadding * 2, 1)
            let n = max(2, Int((usable + Theme.tileSpacing) / (Theme.tileMin + Theme.tileSpacing)))
            let columns = Array(repeating: GridItem(.flexible(), spacing: Theme.tileSpacing, alignment: .top), count: n)
            ScrollView {
                LazyVGrid(columns: columns, spacing: Theme.rowSpacing) {
                    if m.isBuilding { BuildingTile() }
                    ForEach(items) { item in
                        GameTile(item: item, focus: { focused = true })
                    }
                    if !m.isSearching { AddTile() }
                }
                .padding(.horizontal, Theme.gridPadding).padding(.top, 18).padding(.bottom, 32)
                .frame(maxWidth: .infinity)
                .background {
                    // a click on the empty space clears the selection
                    Color.clear.contentShape(.rect).onTapGesture { m.selectedLibraryItem = nil; focused = true }
                }
            }
            .scrollIndicators(.automatic)
            .focusable().focused($focused).focusEffectDisabled()
            .onKeyPress(.leftArrow) { m.moveSelection(by: -1); return .handled }
            .onKeyPress(.rightArrow) { m.moveSelection(by: 1); return .handled }
            .onKeyPress(.upArrow) { m.moveSelection(by: -n); return .handled }
            .onKeyPress(.downArrow) { m.moveSelection(by: n); return .handled }
            .onKeyPress(.return) { if let i = m.selectedItem { m.launch(i.url); return .handled }; return .ignored }
            .onKeyPress(.escape) { if m.selectedLibraryItem != nil { m.selectedLibraryItem = nil; return .handled }; return .ignored }
            .accessibilityIdentifier("library.grid")
        }
    }
}

struct GameTile: View {
    @Environment(AppModel.self) private var m
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let item: LibraryItem
    var focus: () -> Void = {}
    @State private var hovering = false

    private var selected: Bool { m.selectedLibraryItem == item.id }
    private var subtitle: String {
        let size = item.entry.bytes.map { PlanResult.formatBytes($0) }
        let version = item.entry.version.isEmpty ? nil : "v\(item.entry.version)"
        return [version, size].compactMap { $0 }.joined(separator: " · ")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            ZStack(alignment: .bottomTrailing) {
                CoverView(image: AppModel.coverImage(for: item), name: item.entry.name, seed: item.entry.titleId)
                    .shadow(color: .black.opacity(hovering || selected ? 0.32 : 0.18), radius: hovering ? 12 : 7, y: hovering ? 6 : 3)
                if hovering || selected {
                    Button { m.launch(item.url) } label: {
                        Image(systemName: "play.fill").font(.system(size: 15, weight: .bold)).foregroundStyle(.white).frame(width: 40, height: 40)
                            .background(Theme.brand.gradient, in: .circle).shadow(color: .black.opacity(0.35), radius: 6, y: 2)
                    }
                    .buttonStyle(.plain).padding(10)
                    .transition(.opacity.combined(with: .scale(scale: 0.9)))
                    .accessibilityLabel(Text("Play \(item.entry.name)")).accessibilityIdentifier("library.play")
                }
            }
            .overlay(alignment: .topLeading) { BadgeView(badge: m.badge(for: item)).padding(8) }
            .overlay {
                if selected {
                    RoundedRectangle(cornerRadius: Theme.tileRadius + 4, style: .continuous).strokeBorder(Theme.brand, lineWidth: 3).padding(-5)
                }
            }
            .scaleEffect(hovering && !reduceMotion ? 1.02 : 1)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.entry.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                Text(subtitle.isEmpty ? item.entry.titleId : subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .contentShape(.rect)
        .onHover { h in withAnimation(reduceMotion ? nil : .easeOut(duration: 0.14)) { hovering = h } }
        .onTapGesture(count: 2) { m.selectedLibraryItem = item.id; m.launch(item.url) }
        .onTapGesture { focus(); m.selectedLibraryItem = item.id }
        .contextMenu { GameMenu(item: item) }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("\(item.entry.name), \(subtitle)"))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
        .accessibilityAction(named: Text("Play")) { m.launch(item.url) }
        .accessibilityAction(named: Text("Show Details")) { m.selectedLibraryItem = item.id }
        .accessibilityIdentifier("library.card.\(item.entry.titleId)")
    }
}

/// The small status mark in the corner of a cover.
struct BadgeView: View {
    let badge: LibraryBadge
    var body: some View {
        switch badge {
        case .none: EmptyView()
        case .building: Pill(text: String(localized: "Building"), color: Theme.warn, systemImage: "hammer.fill")
        case .compat(let s):
            HStack(spacing: 4) { StatusDot(color: s.color); Text(LocalizedStringKey(s.label)).font(.system(size: 10.5, weight: .semibold)) }
                .padding(.horizontal, 7).padding(.vertical, 3).background(.ultraThinMaterial, in: .capsule)
                .accessibilityElement(children: .combine).accessibilityLabel(Text("Compatibility: \(s.label)"))
        }
    }
}

/// The game being built: a cover with a progress ring. Clicking it opens the build in the inspector.
struct BuildingTile: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            ZStack {
                CoverView(image: nil, name: m.planTitle, seed: m.plan?.base?.titleId ?? m.planTitle, showsName: false)
                RoundedRectangle(cornerRadius: Theme.tileRadius, style: .continuous).fill(.black.opacity(0.55))
                ProgressRing(fraction: m.buildFraction).frame(width: 66, height: 66)
            }
            .overlay {
                if m.panelOpen { RoundedRectangle(cornerRadius: Theme.tileRadius + 4, style: .continuous).strokeBorder(Theme.brand, lineWidth: 3).padding(-5) }
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

/// The last tile: a drop target and a shortcut to the file picker.
struct AddTile: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            Button { m.pickFilesAndAdd() } label: {
                VStack(spacing: 8) {
                    Image(systemName: "plus").font(.system(size: 26, weight: .light))
                    Text("Add a game").font(.caption.weight(.medium))
                }
                .foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity).contentShape(.rect)
            }
            .buttonStyle(.plain)
            .aspectRatio(1, contentMode: .fit)
            .overlay(RoundedRectangle(cornerRadius: Theme.tileRadius, style: .continuous).strokeBorder(.secondary.opacity(0.4), style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
            Text(" ").font(.caption)   // keeps the row height the same as a game's
        }
        .accessibilityLabel(Text("Add a game")).accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("library.addTile")
    }
}

// MARK: - list

struct ListLibrary: View {
    @Environment(AppModel.self) private var m
    let items: [LibraryItem]

    var body: some View {
        @Bindable var m = m
        VStack(spacing: 0) {
            if m.isBuilding { BuildingStrip().padding(.horizontal, Theme.gridPadding).padding(.top, 10) }
            Table(items, selection: $m.selectedLibraryItem) {
                TableColumn("Name") { item in
                    HStack(spacing: 10) {
                        CoverView(image: AppModel.coverImage(for: item), name: item.entry.name, seed: item.entry.titleId, showsName: false, radius: 6).frame(width: 30, height: 30)
                        Text(item.entry.name).fontWeight(.medium).lineLimit(1)
                    }
                }
                .width(min: 180, ideal: 300)
                TableColumn("Version") { item in Text(item.entry.version.isEmpty ? "–" : "v\(item.entry.version)").foregroundStyle(.secondary) }.width(min: 60, ideal: 80, max: 120)
                TableColumn("Size") { item in Text(item.entry.bytes.map { PlanResult.formatBytes($0) } ?? "–").foregroundStyle(.secondary).monospacedDigit() }.width(min: 70, ideal: 90, max: 120)
                TableColumn("Built") { item in Text(item.entry.builtAt, format: .dateTime.day().month(.abbreviated).year()).foregroundStyle(.secondary) }.width(min: 90, ideal: 110, max: 140)
                TableColumn("Status") { item in
                    if case .compat(let s) = m.badge(for: item) { Pill(text: s.label, color: s.color) }
                    else { Text("Ready").foregroundStyle(.secondary) }
                }
                .width(min: 90, ideal: 120, max: 160)
                TableColumn("") { item in
                    Button { m.launch(item.url) } label: { Image(systemName: "play.fill") }
                        .buttonStyle(.borderless).foregroundStyle(Theme.brand)
                        .accessibilityLabel(Text("Play \(item.entry.name)"))
                }
                .width(34)
            }
            .contextMenu(forSelectionType: UUID.self) { ids in
                if let id = ids.first, let item = m.presentLibrary.first(where: { $0.id == id }) { GameMenu(item: item) }
            } primaryAction: { ids in
                if let id = ids.first, let item = m.presentLibrary.first(where: { $0.id == id }) { m.launch(item.url) }
            }
            .scrollContentBackground(.hidden)
            .accessibilityIdentifier("library.list")
        }
    }
}

/// List mode has no tile for the build in flight: this strip stands in for it.
struct BuildingStrip: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Button { m.panelOpen = true } label: {
            HStack(spacing: 12) {
                ProgressView(value: m.buildFraction).progressViewStyle(.circular).controlSize(.small)
                Text("Building \(m.planTitle)").fontWeight(.semibold)
                Spacer()
                if let s = m.buildStarted { Text(s, style: .timer).monospacedDigit().foregroundStyle(.secondary) }
                Text("\(Int(m.buildFraction * 100))%").monospacedDigit()
            }
            .padding(.horizontal, 14).padding(.vertical, 10).background(Theme.warn.opacity(0.14), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        }
        .buttonStyle(.plain).accessibilityIdentifier("library.building")
    }
}

// MARK: - menus and states

struct GameMenu: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    var body: some View {
        Button("Play") { m.launch(item.url) }
        Button("Play with Debug") { m.playWithDebug(item) }
        Divider()
        Button("Show Details") { m.selectedLibraryItem = item.id }
        Button("Show in Finder") { m.reveal(item.url) }
        Button("Open Saves Folder") { m.openSaves(item) }
        Divider()
        Button("Update or Add DLC…") { m.rebuildWithNewFiles(item) }
        Button("Rebuild with Mods…") { m.rebuildWithMods(item) }
        Divider()
        Button("Move to Trash", role: .destructive) { m.trashFromLibrary(item) }
        Button("Remove from Library") { m.removeFromLibrary(item.id) }
    }
}

/// No games yet: what to do, with a drop area.
struct EmptyLibrary: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 18) {
            AppMark(size: 84)
            VStack(spacing: 6) {
                Text("Add your first game").font(.system(size: 24, weight: .semibold))
                Text("Drop an NSP, XCI, NSZ or XCZ file here, or a folder with the game, its updates and DLC. Napple turns it into a native Mac app that lives in this library.")
                    .foregroundStyle(.secondary).multilineTextAlignment(.center).frame(maxWidth: 420)
            }
            HStack(spacing: 10) {
                Button { m.pickFilesAndAdd() } label: { Label("Choose Files…", systemImage: "plus") }
                    .buttonStyle(.glassProminent).controlSize(.large).accessibilityIdentifier("library.choose")
                if m.sets.isEmpty {
                    Button("Set Up Keys") { m.showKeysSetup() }.buttonStyle(.glass).controlSize(.large)
                }
            }
            Text("Your files stay on this Mac. Nothing is uploaded.").font(.caption).foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous).strokeBorder(.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1.5, dash: [8, 6])).padding(Theme.gridPadding))
        .accessibilityElement(children: .contain).accessibilityIdentifier("library.empty")
    }
}

struct NoResults: View {
    let query: String
    var body: some View {
        ContentUnavailableView.search(text: query).frame(maxWidth: .infinity, maxHeight: .infinity).padding(.bottom, 40)
    }
}

/// Keys were deleted after setup: building is paused, games still play.
struct KeysRemovedBanner: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: Tone.warn.symbol).foregroundStyle(Theme.warn).accessibilityHidden(true)
            Text("Your keys were removed, so building is paused. Your games still play.").frame(maxWidth: .infinity, alignment: .leading)
            Button("Add Keys") { m.showKeysSetup() }.buttonStyle(.glass)
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .background(Theme.warn.opacity(0.14), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        .accessibilityElement(children: .contain)
    }
}
