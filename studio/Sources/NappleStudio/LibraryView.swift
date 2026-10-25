import SwiftUI
import AppKit
import NappleStudioCore

/// Home: every Mac app Studio has built, ready to launch.
struct LibraryView: View {
    @Environment(AppModel.self) private var m
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 12, alignment: .top), count: 2)

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .bottom) {
                ScreenTitle(title: "Your games", subtitle: m.libraryItems.isEmpty ? "Nothing built yet." : countLine, size: 28)
                Spacer()
                Button { m.showAdd() } label: { Label("Add game", systemImage: "plus") }
                    .buttonStyle(PrimaryPill(height: 34, fontSize: 13))
                    .accessibilityLabel("Add a game")
            }
            .padding(.horizontal, Metrics.pad)

            if m.libraryItems.isEmpty {
                EmptyLibrary()
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 12) {
                        ForEach(m.libraryItems) { LibraryCard(item: $0) }
                        AddTile()
                    }
                    .padding(.horizontal, Metrics.pad).padding(.bottom, 4)
                }
                .scrollIndicators(.hidden)
            }
            LegalFooter().padding(.bottom, 4)
        }
        .padding(.top, 6).padding(.bottom, 10)
    }

    private var countLine: String {
        let n = m.libraryItems.count
        let missing = m.libraryItems.filter { $0.status != .present }.count
        let base = "\(n) built \(n == 1 ? "app" : "apps") · launch from here, or add another."
        return missing == 0 ? base : "\(n) built \(n == 1 ? "app" : "apps") · \(missing) can't be found."
    }
}

struct EmptyLibrary: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "gamecontroller").font(.system(size: 32)).foregroundStyle(Palette.red)
            Text("Build your first game").font(.system(size: 17, weight: .semibold))
            Text("Drag .nsp, .xci, .nsz or .xcz files anywhere into this window, or pick them yourself.")
                .font(.system(size: 13)).foregroundStyle(Palette.secondary).multilineTextAlignment(.center)
            Button("Choose Files…") { m.pickFilesAndAdd() }.buttonStyle(SecondaryPill(height: 30, fontSize: 12))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(24)
    }
}

struct AddTile: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        Button { m.showAdd() } label: {
            VStack(spacing: 8) {
                Image(systemName: "plus").font(.system(size: 16, weight: .bold)).foregroundStyle(Palette.pink)
                    .frame(width: 36, height: 36).background(Palette.red.opacity(0.22), in: Circle())
                Text("Add a game").font(.system(size: 13, weight: .medium))
            }
            .frame(maxWidth: .infinity, minHeight: 118)
            .overlay(RoundedRectangle(cornerRadius: 24, style: .continuous)
                .strokeBorder(Palette.strokeStrong, style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
            .contentShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Add a game")
    }
}

struct LibraryCard: View {
    @Environment(AppModel.self) private var m
    let item: LibraryItem
    @State private var confirmTrash = false

    private var present: Bool { item.status == .present }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 12) {
                AppIconView(url: item.url, size: 56, radius: 12)
                    .opacity(present ? 1 : 0.55)
                VStack(alignment: .leading, spacing: 4) {
                    Text(item.entry.name).font(.system(size: 13, weight: .semibold)).lineLimit(2)
                        .fixedSize(horizontal: false, vertical: true)
                    Text(detail).font(.system(size: 11)).foregroundStyle(Palette.tertiary).monospacedDigit()
                    if present { CompatBadge(entry: item.compat) }
                    else {
                        Label(item.status == .replaced ? "Not this game any more" : "Can't find this app", systemImage: "exclamationmark.triangle.fill")
                            .font(.system(size: 11, weight: .medium)).foregroundStyle(Palette.amber)
                    }
                }
            }
            Spacer(minLength: 0)
            HStack(spacing: 8) {
                if present {
                    Button { m.launch(item.url) } label: { Text("Launch").frame(maxWidth: .infinity) }
                        .buttonStyle(PrimaryPill(height: 30, fontSize: 12))
                        .accessibilityLabel("Launch \(item.entry.name)")
                } else {
                    Button { m.locate(item) } label: { Text("Locate…").frame(maxWidth: .infinity) }
                        .buttonStyle(SecondaryPill(height: 30, fontSize: 12))
                        .accessibilityLabel("Locate \(item.entry.name)")
                }
                Menu { actions } label: {
                    Image(systemName: "ellipsis").font(.system(size: 12, weight: .bold))
                        .frame(width: 30, height: 30).background(Palette.fill, in: Circle())
                        .overlay(Circle().strokeBorder(Palette.strokeStrong))
                }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
                .accessibilityLabel("More actions for \(item.entry.name)")
            }
        }
        .frame(maxWidth: .infinity, minHeight: 118, alignment: .topLeading)
        .card(radius: 24, padding: 12)
        .contextMenu { actions }
        .confirmationDialog("Move \(item.entry.name) to the Trash?", isPresented: $confirmTrash) {
            Button("Move to Trash", role: .destructive) { m.trashFromLibrary(item) }
        } message: { Text("The app and its game data are deleted from this Mac.") }
    }

    @ViewBuilder private var actions: some View {
        if present {
            Button("Show in Finder") { m.reveal(item.url) }
            Button("Install to /Applications") { m.installToApplications(item.url) }
            Divider()
        } else {
            Button("Locate…") { m.locate(item) }
        }
        Button("Remove from Library") { m.removeFromLibrary(item.id) }
        if present { Button("Move to Trash…", role: .destructive) { confirmTrash = true } }
    }

    private var detail: String {
        var parts: [String] = []
        if !item.entry.version.isEmpty { parts.append(item.entry.version) }
        if let b = item.entry.bytes, b > 0 { parts.append(PlanResult.formatBytes(b)) }
        return parts.isEmpty ? "Built " + item.entry.builtAt.formatted(date: .abbreviated, time: .omitted) : parts.joined(separator: " · ")
    }
}

/// Pick files to add (also what dragging over the window shows).
struct AddView: View {
    @Environment(AppModel.self) private var m

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            ScreenTitle(title: "Turn a game dump into a Mac app.",
                        subtitle: "Drop the base game, plus any updates and DLC. Studio reads them, shows you what's inside, then builds a standalone app.",
                        size: 26)
            VStack(spacing: 10) {
                Image(systemName: "tray.and.arrow.down").font(.system(size: 27)).foregroundStyle(Palette.red)
                    .frame(width: 56, height: 56).background(Palette.fillStrong, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                Text("Drop your game here").font(.system(size: 19, weight: .semibold))
                HStack(spacing: 8) {
                    ForEach([".nsp", ".xci", ".nsz", ".xcz", "folders"], id: \.self) { t in
                        Text(t).font(.system(size: 11, design: t == "folders" ? .default : .monospaced))
                            .padding(.horizontal, 10).padding(.vertical, 4).background(Palette.fillStrong, in: Capsule())
                    }
                }
                Button("Choose Files…") { m.pickFilesAndAdd() }
                    .buttonStyle(PrimaryPill(height: 36, fontSize: 13)).padding(.top, 2)
                if let e = m.errorText { Banner(text: e, tone: .warn).frame(maxWidth: 520) }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Palette.fill.opacity(0.6)))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                .strokeBorder(Palette.strokeStrong, style: StrokeStyle(lineWidth: 1.5, dash: [7, 6])))
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Drop zone for game files")
            LegalFooter()
        }
        .padding(.horizontal, Metrics.pad).padding(.top, 6).padding(.bottom, 10)
    }
}
