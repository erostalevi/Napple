import SwiftUI
import AppKit
import NappleStudioCore

/// Small uppercase section label used above every group in Settings.
struct SettingsHeader: View {
    let title: LocalizedStringKey
    init(_ title: LocalizedStringKey) { self.title = title }
    var body: some View {
        Text(title).font(.caption.weight(.semibold)).textCase(.uppercase).foregroundStyle(.secondary).tracking(0.6)
    }
}

/// Explanatory text under a group.
struct SettingsFootnote: View {
    let text: LocalizedStringKey
    var tone: Color = .secondary
    init(_ text: LocalizedStringKey, tone: Color = .secondary) { self.text = text; self.tone = tone }
    var body: some View {
        Text(text).font(.caption).foregroundStyle(tone).fixedSize(horizontal: false, vertical: true)
    }
}

/// An inline notice with an icon and a tinted background.
struct SettingsBanner: View {
    enum Tone { case info, good, warn, bad }
    let text: String
    var tone: Tone = .info
    var busy = false
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            if busy { ProgressView().controlSize(.small) } else { Image(systemName: symbol).foregroundStyle(color) }
            Text(text).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .font(.callout)
        .padding(10)
        .background(color.opacity(0.14), in: .rect(cornerRadius: 10, style: .continuous))
        .accessibilityElement(children: .combine)
    }
    private var symbol: String {
        switch tone { case .info: "info.circle"; case .good: "checkmark.circle"; case .warn: "exclamationmark.triangle"; case .bad: "xmark.octagon" }
    }
    private var color: Color {
        switch tone { case .info: .accentColor; case .good: .green; case .warn: .orange; case .bad: .red }
    }
}

/// A selectable row with a radio indicator, a title and an optional explanation.
struct SettingsRadioRow: View {
    let title: LocalizedStringKey
    var detail: LocalizedStringKey?
    let selected: Bool
    let identifier: String
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: selected ? "largecircle.fill.circle" : "circle")
                    .foregroundStyle(selected ? Color.accentColor : .secondary).font(.title3).frame(width: 22)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).foregroundStyle(.primary)
                    if let detail { Text(detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
                }
                Spacer(minLength: 0)
            }
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
        .accessibilityIdentifier(identifier)
    }
}

/// A "Choose…" file or folder row bound to a string of system.json. Commits on Return, on focus loss and after choosing.
struct SettingsPathRow: View {
    let title: LocalizedStringKey
    @Binding var value: String
    var folder = false
    var placeholder: LocalizedStringKey = ""
    var identifier: String
    var onCommit: () -> Void
    @FocusState private var focused: Bool
    var body: some View {
        LabeledContent(title) {
            HStack(spacing: 6) {
                TextField("", text: $value, prompt: Text(placeholder)).labelsHidden().textFieldStyle(.roundedBorder).focused($focused)
                    .onSubmit(onCommit).accessibilityIdentifier("\(identifier).field")
                Button("Choose…") {
                    if let u = pick(files: !folder, dirs: folder, message: String(localized: "Choose")).first { value = u.path; onCommit() }
                }
                .accessibilityIdentifier("\(identifier).choose")
                if !value.isEmpty {
                    Button { value = ""; onCommit() } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.borderless).foregroundStyle(.secondary)
                        .help("Clear")
                        .accessibilityLabel("Clear")
                        .accessibilityIdentifier("\(identifier).clear")
                }
            }
        }
        .onChange(of: focused) { _, now in if !now { onCommit() } }
    }
}

/// system.json editing for the Mii, Amiibo and Online tabs: every change reloads the file first, so tabs never overwrite each other's keys.
@MainActor
@Observable
final class SettingsSystemModel {
    var config = SystemConfig.load()
    var message: String?
    func reload() { config = SystemConfig.load() }
    /// Saves only the key `path` points at, on top of what is on disk now.
    func commit(_ path: WritableKeyPath<SystemConfig, String>) {
        var disk = SystemConfig.load()
        disk[keyPath: path] = config[keyPath: path]
        do { try disk.save(); message = nil; config = disk } catch { message = error.localizedDescription }
    }
    var mii: MiiStore { MiiStore(folder: config.effectiveMiiFolder) }
}

/// Wraps a tab's form in the fixed 620 pt centred column on a translucent background.
struct SettingsPage<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        Form { content }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)
            .frame(maxWidth: 620)
            .frame(maxWidth: .infinity)
    }
}
