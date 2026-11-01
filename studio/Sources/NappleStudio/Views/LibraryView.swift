import SwiftUI
import AppKit
import NappleStudioCore

struct LibraryView: View {
    @Environment(AppModel.self) private var m
    private let columns = [GridItem(.adaptive(minimum: 180, maximum: 240), spacing: 16)]

    var body: some View {
        @Bindable var m = m
        Group {
            if m.libraryItems.isEmpty {
                VStack(spacing: 14) {
                    Image(systemName: "gamecontroller").font(.system(size: 56, weight: .light)).foregroundStyle(.tint)
                    Text("No games yet").font(.title.weight(.semibold))
                    Text("Drop a game file anywhere in this window. Napple turns it into a native Mac app that lives here.")
                        .foregroundStyle(.secondary).multilineTextAlignment(.center).frame(maxWidth: 380)
                    Button("Add Game…") { m.pickFilesAndAdd() }.buttonStyle(.glassProminent).controlSize(.large)
                    if m.needsSetup { Button("Set Up Keys…") { m.pane = .add } }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 16) {
                        ForEach(m.filteredLibrary) { item in
                            GameCard(item: item)
                                .contextMenu { GameMenu(item: item) }
                                .accessibilityIdentifier("library.card.\(item.entry.titleId)")
                        }
                    }
                    .padding(.horizontal, 24).padding(.bottom, 20)
                }
                .scrollIndicators(.hidden)
            }
        }
        .focusedSceneValue(\.selectedGame, m.libraryItems.first { $0.id == m.selectedLibraryItem })
        .sheet(item: $m.runItem) { RunLogView(item: $0) }
    }
}

struct GameCard: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ZStack {
                RoundedRectangle(cornerRadius: 16, style: .continuous).fill(.quaternary)
                if let img = AppModel.coverImage(for: item) {
                    Image(nsImage: img).resizable().scaledToFit().padding(8)
                } else {
                    Image(systemName: item.status == .present ? "gamecontroller" : "questionmark.app.dashed").font(.system(size: 40, weight: .light)).foregroundStyle(.secondary)
                }
            }
            .aspectRatio(1, contentMode: .fit)
            .opacity(item.status == .present ? 1 : 0.5)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    if let c = item.compat { StatusDot(color: c.status.color).help(c.status.label) }
                    Text(item.entry.name).font(.headline).lineLimit(1)
                }
                Text(item.status != .present ? (item.status == .missing ? "Missing" : "Replaced") : (item.entry.version.isEmpty ? item.entry.titleId : "v\(item.entry.version)") + (item.entry.bytes.map { " · " + PlanResult.formatBytes($0) } ?? ""))
                    .font(.caption).foregroundStyle(item.status == .present ? Color.secondary : Color.orange).lineLimit(1)
            }
            if item.status == .present {
                Button { m.launch(item.url) } label: { Label("Play", systemImage: "play.fill").frame(maxWidth: .infinity) }
                    .buttonStyle(.glassProminent).accessibilityIdentifier("library.play")
            } else {
                Button { m.locate(item) } label: { Label("Locate…", systemImage: "magnifyingglass").frame(maxWidth: .infinity) }.buttonStyle(.glass)
            }
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(.separator.opacity(0.5)))
        .modifier(HoverLift(scale: 1.015))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("\(item.entry.name), version \(item.entry.version)\(item.status == .present ? "" : ", missing")")
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

struct SelectedGameKey: FocusedValueKey { typealias Value = LibraryItem }
extension FocusedValues { var selectedGame: LibraryItem? { get { self[SelectedGameKey.self] } set { self[SelectedGameKey.self] = newValue } } }
