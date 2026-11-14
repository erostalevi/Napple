import SwiftUI
import AppKit
import NappleStudioCore

extension CompatStatus {
    var color: Color {
        switch self { case .perfect, .playable: Theme.good; case .partial: .yellow; case .menus, .boots: Theme.warn; case .nothing: Theme.bad; case .unknown: .secondary }
    }
    /// Statuses worth a badge on the tile: anything not known to be fine.
    var needsAttention: Bool {
        switch self { case .perfect, .playable, .unknown: false; default: true }
    }
}

/// Compatibility status as a dot and a word.
struct CompatBadge: View {
    let entry: CompatEntry
    var body: some View {
        HStack(spacing: 6) { StatusDot(color: entry.status.color); Text(entry.status.label).font(.caption) }
            .accessibilityElement(children: .combine)
    }
}

/// Primary actions in the inspector and dialogs.
struct ActionButton: View {
    enum Kind { case primary, secondary, destructive }
    let title: LocalizedStringKey
    var systemImage: String?
    var kind: Kind = .secondary
    var id: String?
    var action: () -> Void
    var body: some View {
        Button(action: action) {
            Group { if let systemImage { Label(title, systemImage: systemImage) } else { Text(title) } }.frame(maxWidth: .infinity)
        }
        .controlSize(.large)
        .modifier(ActionStyle(kind: kind))
        .accessibilityIdentifier(id ?? "")
    }
}

private struct ActionStyle: ViewModifier {
    let kind: ActionButton.Kind
    func body(content: Content) -> some View {
        switch kind {
        case .primary: content.buttonStyle(.glassProminent)
        case .secondary: content.buttonStyle(.glass)
        case .destructive: content.buttonStyle(.glass).tint(Theme.bad)
        }
    }
}
