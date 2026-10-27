import SwiftUI
import AppKit
import NappleStudioCore

struct LibraryView: View {
    @Environment(AppModel.self) private var m
    private let columns = [GridItem(.adaptive(minimum: 170, maximum: 220), spacing: 18)]

    var body: some View {
        @Bindable var m = m
        Group {
            if m.libraryItems.isEmpty {
                ContentUnavailableView {
                    Label("No games yet", systemImage: "square.grid.2x2")
                } description: {
                    Text("Drop a game file anywhere in this window, or press ⌘O. Napple turns it into a native Mac app that lives here.")
                } actions: {
                    Button("Add Game…") { m.pickFilesAndAdd() }.buttonStyle(.glassProminent)
                    if m.needsSetup { Button("Set Up Keys…") { m.pane = .add } }
                }
            } else if m.filteredLibrary.isEmpty {
                ContentUnavailableView.search(text: m.searchText)
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 18) {
                        ForEach(m.filteredLibrary) { item in
                            GameCard(item: item, selected: m.selectedLibraryItem == item.id)
                                .onTapGesture { m.selectedLibraryItem = item.id }
                                .onTapGesture(count: 2) { m.launch(item.url) }
                                .contextMenu { GameMenu(item: item) }
                                .accessibilityIdentifier("library.card.\(item.entry.titleId)")
                        }
                    }
                    .padding(24)
                }
                .scrollEdgeEffectStyle(.soft, for: .top)
            }
        }
        .searchable(text: $m.searchText, placement: .toolbar, prompt: "Search games")
        .overlay(alignment: .bottom) {
            if let id = m.selectedLibraryItem, let item = m.libraryItems.first(where: { $0.id == id }) { GameDetailBar(item: item) }
        }
        .focusedSceneValue(\.selectedGame, m.libraryItems.first { $0.id == m.selectedLibraryItem })
        .sheet(item: $m.runItem) { RunLogView(item: $0) }
        .onDeleteCommand { if let id = m.selectedLibraryItem { m.removeFromLibrary(id) } }
    }
}

struct GameCard: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    let selected: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack {
                RoundedRectangle(cornerRadius: 22, style: .continuous).fill(.quaternary)
                if let img = AppModel.coverImage(for: item) {
                    Image(nsImage: img).resizable().scaledToFit().padding(10)
                } else {
                    Image(systemName: item.status == .present ? "gamecontroller" : "questionmark.app.dashed").font(.system(size: 44)).foregroundStyle(.secondary)
                }
            }
            .aspectRatio(1, contentMode: .fit)
            .overlay(alignment: .topTrailing) {
                if let c = item.compat { StatusDot(color: c.status.color).padding(10).help(c.status.label) }
            }
            .opacity(item.status == .present ? 1 : 0.5)
            Text(item.entry.name).font(.headline).lineLimit(1)
            HStack(spacing: 6) {
                Text(item.entry.version.isEmpty ? item.entry.titleId : "v\(item.entry.version)")
                if let b = item.entry.bytes { Text("·"); Text(PlanResult.formatBytes(b)) }
                if item.status != .present { Text("·"); Text(item.status == .missing ? "Missing" : "Replaced").foregroundStyle(.orange) }
            }
            .font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
        .padding(10)
        .background(selected ? AnyShapeStyle(.selection.opacity(0.18)) : AnyShapeStyle(.clear), in: .rect(cornerRadius: 26, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 26, style: .continuous).strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 2))
        .contentShape(.rect(cornerRadius: 26, style: .continuous))
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(item.entry.name), version \(item.entry.version)\(item.status == .present ? "" : ", missing")")
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
    }
}

struct GameMenu: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    var body: some View {
        if item.status == .present {
            Button("Play") { m.launch(item.url) }
            Button("Play with Log…") { m.runItem = item }
            Button("Show in Finder") { m.reveal(item.url) }
            Button("Copy to Applications") { if let u = item.url { m.installToApplications(u) } }
            Divider()
            Button("Move to Trash", role: .destructive) { m.trashFromLibrary(item) }
        } else {
            Button("Locate…") { m.locate(item) }
        }
        Button("Remove from Library") { m.removeFromLibrary(item.id) }
    }
}

/// Bottom bar with the selected game's actions.
struct GameDetailBar: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    var body: some View {
        GlassEffectContainer {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.entry.name).font(.headline)
                    HStack(spacing: 6) {
                        Text(item.entry.titleId).monospaced()
                        if let c = item.compat { Text("·"); Text(c.status.label).foregroundStyle(c.status.color) }
                        Text("·"); Text(item.entry.builtAt, style: .date)
                    }.font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if item.status == .present {
                    Button { m.reveal(item.url) } label: { Label("Show in Finder", systemImage: "folder") }.buttonStyle(.glass)
                    Button { m.launch(item.url) } label: { Label("Play", systemImage: "play.fill") }.buttonStyle(.glassProminent).keyboardShortcut(.return, modifiers: [])
                        .accessibilityIdentifier("library.play")
                } else {
                    Button { m.locate(item) } label: { Label("Locate…", systemImage: "magnifyingglass") }.buttonStyle(.glassProminent)
                }
                Button { m.selectedLibraryItem = nil } label: { Image(systemName: "xmark") }.buttonStyle(.glass).accessibilityLabel("Deselect")
            }
            .padding(14)
            .glassEffect(.regular, in: .rect(cornerRadius: 22, style: .continuous))
        }
        .padding(18)
        .transition(.move(edge: .bottom).combined(with: .opacity))
    }
}

struct SelectedGameKey: FocusedValueKey { typealias Value = LibraryItem }
extension FocusedValues { var selectedGame: LibraryItem? { get { self[SelectedGameKey.self] } set { self[SelectedGameKey.self] = newValue } } }
