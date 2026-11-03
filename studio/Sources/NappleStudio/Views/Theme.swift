import SwiftUI
import AppKit

/// The look of the main window: a translucent window, quiet translucent surfaces, one accent, status colours that always mean the same thing.
enum Theme {
    static let tileRadius: CGFloat = 14
    static let cardRadius: CGFloat = 10
    static let panelWidth: CGFloat = 420
    static let windowSize = CGSize(width: 1000, height: 680)

    /// Surfaces are tints of the foreground colour, so they follow light and dark mode and let the window material through.
    static let surface = Color.primary.opacity(0.07)
    static let surfaceStrong = Color.primary.opacity(0.12)
    static let hairline = Color.primary.opacity(0.10)

    static let good = Color(nsColor: .systemGreen)
    static let warn = Color(nsColor: .systemOrange)
    static let bad = Color(nsColor: .systemRed)
}

/// The window's translucent background (vibrancy: the desktop shows through, blurred).
struct WindowMaterial: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let v = NSVisualEffectView()
        v.material = .underWindowBackground
        v.blendingMode = .behindWindow
        v.state = .active
        return v
    }
    func updateNSView(_ v: NSVisualEffectView, context: Context) {}
}

/// Small uppercase label above a group.
struct SectionLabel: View {
    let text: LocalizedStringKey
    init(_ text: LocalizedStringKey) { self.text = text }
    var body: some View {
        Text(text).font(.caption.weight(.semibold)).foregroundStyle(.secondary).textCase(.uppercase).padding(.leading, 2)
    }
}

/// A rounded translucent group of rows separated by hairlines.
struct RowGroup<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        VStack(spacing: 0) { content }
            .background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
    }
}

/// One row inside a RowGroup: leading label, trailing content.
struct RowLine<Trailing: View>: View {
    let label: LocalizedStringKey
    var divider = true
    @ViewBuilder var trailing: Trailing
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Text(label).lineLimit(1)
                Spacer(minLength: 8)
                trailing
            }
            .padding(.horizontal, 12).padding(.vertical, 9)
            if divider { Divider().overlay(Theme.hairline).padding(.leading, 12) }
        }
    }
}

/// A coloured message line: info, warning, error, success.
struct Notice: View {
    var text: String
    var tone: Tone = .info
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 9) {
            Circle().fill(color).frame(width: 8, height: 8).alignmentGuide(.firstTextBaseline) { $0[.bottom] - 1 }.accessibilityHidden(true)
            Text(text).font(.callout).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12).padding(.vertical, 9)
        .background(color.opacity(0.14), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        .accessibilityElement(children: .combine)
    }
    private var color: Color {
        switch tone { case .info: .accentColor; case .good: Theme.good; case .warn: Theme.warn; case .bad: Theme.bad }
    }
}

/// A flat cover tile: the game's icon when there is one, else its name on a colour taken from the title id.
struct CoverView: View {
    let image: NSImage?
    let name: String
    let seed: String
    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .bottomLeading) {
                if let image {
                    Image(nsImage: image).resizable().scaledToFill().frame(width: g.size.width, height: g.size.height).clipped()
                } else {
                    Rectangle().fill(Self.color(for: seed))
                    Text(name.uppercased()).font(.system(size: max(11, g.size.width * 0.1), weight: .heavy)).foregroundStyle(.white)
                        .lineLimit(3).minimumScaleFactor(0.6).padding(g.size.width * 0.07)
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .clipShape(.rect(cornerRadius: Theme.tileRadius, style: .continuous))
    }
    /// Stable, readable (white text passes 4.5:1) colours.
    static func color(for seed: String) -> Color {
        let palette: [Color] = [Color(red: 0.70, green: 0.15, blue: 0.12), Color(red: 0.12, green: 0.43, blue: 0.55), Color(red: 0.18, green: 0.42, blue: 0.31),
                                Color(red: 0.36, green: 0.29, blue: 0.60), Color(red: 0.54, green: 0.25, blue: 0.69), Color(red: 0.55, green: 0.34, blue: 0.10)]
        let h = seed.unicodeScalars.reduce(5381) { ($0 &* 33) &+ Int($1.value) }
        return palette[abs(h) % palette.count]
    }
}

/// Primary / secondary / destructive buttons used in panels and dialogs.
struct PanelButton: View {
    enum Kind { case primary, secondary, destructive }
    let title: LocalizedStringKey
    var kind: Kind = .secondary
    var systemImage: String?
    var action: () -> Void
    var body: some View {
        Button(action: action) {
            if let systemImage { Label(title, systemImage: systemImage) } else { Text(title) }
        }
        .controlSize(.large)
        .modifier(PanelButtonStyle(kind: kind))
    }
}

private struct PanelButtonStyle: ViewModifier {
    let kind: PanelButton.Kind
    func body(content: Content) -> some View {
        switch kind {
        case .primary: content.buttonStyle(.glassProminent)
        case .secondary: content.buttonStyle(.glass)
        case .destructive: content.buttonStyle(.glass).tint(Theme.bad)
        }
    }
}

/// A round icon-only button in the header.
struct HeaderIconButton: View {
    let symbol: String
    let label: LocalizedStringKey
    var action: () -> Void
    var body: some View {
        Button(action: action) { Image(systemName: symbol).frame(width: 18, height: 18) }
            .buttonStyle(.glass).buttonBorderShape(.circle).controlSize(.regular)
            .help(Text(label)).accessibilityLabel(Text(label))
    }
}
