import SwiftUI
import AppKit

/// The look of Napple Studio: one brand colour (the coral of the app icon), quiet surfaces that are tints of the foreground colour
/// (so they follow light and dark mode), status colours that always mean the same thing, and a short list of sizes. Nothing outside this
/// file picks a raw colour, radius or spacing for a layout decision.
enum Theme {
    // MARK: sizes
    enum Space { static let xs: CGFloat = 4, s: CGFloat = 8, m: CGFloat = 12, l: CGFloat = 16, xl: CGFloat = 24, xxl: CGFloat = 32 }
    static let tileRadius: CGFloat = 16
    static let cardRadius: CGFloat = 12
    static let controlRadius: CGFloat = 8
    /// The main window opens at `windowSize` and cannot be made smaller than `minWindowSize`.
    static let windowSize = CGSize(width: 1120, height: 740)
    static let minWindowSize = CGSize(width: 820, height: 560)
    static let inspectorWidth: (min: CGFloat, ideal: CGFloat, max: CGFloat) = (360, 400, 480)
    /// Library tiles: as many columns as fit, never narrower than `tileMin`.
    static let tileMin: CGFloat = 156
    static let tileSpacing: CGFloat = 22
    static let gridPadding: CGFloat = 28
    static let rowSpacing: CGFloat = 26

    // MARK: colours
    /// The coral of the app icon.
    static let brand = Color(red: 0.93, green: 0.27, blue: 0.33)
    static let brandDeep = Color(red: 0.84, green: 0.16, blue: 0.34)

    static let surface = Color.primary.opacity(0.06)
    static let surfaceStrong = Color.primary.opacity(0.11)
    static let hairline = Color.primary.opacity(0.10)

    static let good = Color(nsColor: .systemGreen)
    static let warn = Color(nsColor: .systemOrange)
    static let bad = Color(nsColor: .systemRed)

    /// The log wells: the system text background, so the text keeps its contrast in both appearances.
    static let well = Color(nsColor: .textBackgroundColor).opacity(0.65)
}

// MARK: - window backdrop

/// Plain window background with a faint brand glow in the top corner: enough to feel like Napple, quiet enough for covers to lead.
struct Backdrop: View {
    @Environment(\.colorScheme) private var scheme
    var body: some View {
        ZStack {
            Color(nsColor: .windowBackgroundColor)
            RadialGradient(colors: [Theme.brand.opacity(scheme == .dark ? 0.16 : 0.10), .clear], center: .topLeading, startRadius: 0, endRadius: 620)
            RadialGradient(colors: [Theme.brandDeep.opacity(scheme == .dark ? 0.08 : 0.05), .clear], center: .bottomTrailing, startRadius: 0, endRadius: 520)
        }
        .ignoresSafeArea()
    }
}

/// Glass where the system draws it, an opaque material when the user asked for less transparency.
struct GlassSurface: ViewModifier {
    @Environment(\.accessibilityReduceTransparency) private var reduce
    var radius: CGFloat = Theme.cardRadius
    func body(content: Content) -> some View {
        if reduce {
            content.background(Color(nsColor: .controlBackgroundColor), in: .rect(cornerRadius: radius, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Theme.hairline))
        } else {
            content.glassEffect(.regular, in: .rect(cornerRadius: radius, style: .continuous))
        }
    }
}
extension View {
    func glassSurface(radius: CGFloat = Theme.cardRadius) -> some View { modifier(GlassSurface(radius: radius)) }
}

/// A capsule glass chip used for the toast and small floating labels.
struct GlassCapsule: ViewModifier {
    @Environment(\.accessibilityReduceTransparency) private var reduce
    func body(content: Content) -> some View {
        if reduce { content.background(Color(nsColor: .controlBackgroundColor), in: .capsule).overlay(Capsule().strokeBorder(Theme.hairline)) }
        else { content.glassEffect(.regular, in: .capsule) }
    }
}
extension View { func glassCapsule() -> some View { modifier(GlassCapsule()) } }

// MARK: - small building blocks

/// "BETA": the mark that tells this build is not final. Used in the About window, the welcome screen and the window subtitle's neighbour.
struct BetaBadge: View {
    var body: some View {
        Text("BETA").font(.system(size: 10, weight: .heavy)).tracking(1)
            .foregroundStyle(.white).padding(.horizontal, 6).padding(.vertical, 2)
            .background(Theme.brand.gradient, in: .capsule)
            .accessibilityLabel(Text("Beta"))
    }
}

/// Small uppercase label above a group.
struct SectionLabel: View {
    let text: LocalizedStringKey
    init(_ text: LocalizedStringKey) { self.text = text }
    var body: some View {
        Text(text).font(.caption.weight(.semibold)).foregroundStyle(.secondary).textCase(.uppercase).tracking(0.5).padding(.leading, 2)
            .accessibilityAddTraits(.isHeader)
    }
}

/// A rounded quiet group of rows separated by hairlines.
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

enum Tone { case info, good, warn, bad
    var color: Color {
        switch self { case .info: Theme.brand; case .good: Theme.good; case .warn: Theme.warn; case .bad: Theme.bad }
    }
    var symbol: String {
        switch self { case .info: "info.circle.fill"; case .good: "checkmark.circle.fill"; case .warn: "exclamationmark.triangle.fill"; case .bad: "xmark.octagon.fill" }
    }
}

/// A coloured message: info, warning, error, success. The icon carries the meaning too, so colour is never the only signal.
struct Notice: View {
    var text: String
    var tone: Tone = .info
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 9) {
            Image(systemName: tone.symbol).foregroundStyle(tone.color).font(.callout).accessibilityHidden(true)
            Text(text).font(.callout).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12).padding(.vertical, 9)
        .background(tone.color.opacity(0.13), in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

struct StatusDot: View {
    var color: Color
    var body: some View { Circle().fill(color).frame(width: 8, height: 8).accessibilityHidden(true) }
}

/// A small pill: a dot and a word on a tinted background (library status badges, key-set "In use").
struct Pill: View {
    let text: String
    var color: Color = .secondary
    var systemImage: String?
    var body: some View {
        HStack(spacing: 4) {
            if let systemImage { Image(systemName: systemImage).font(.system(size: 9, weight: .bold)) }
            Text(text).font(.system(size: 10.5, weight: .semibold))
        }
        .foregroundStyle(color).padding(.horizontal, 7).padding(.vertical, 3)
        .background(color.opacity(0.15), in: .capsule)
        .accessibilityElement(children: .combine)
    }
}

/// The game's cover: its icon when the build stored one, else a generated tile (gradient from the title id, monogram, name).
struct CoverView: View {
    let image: NSImage?
    let name: String
    let seed: String
    var showsName = true
    var radius: CGFloat = Theme.tileRadius
    var body: some View {
        GeometryReader { g in
            ZStack {
                if let image {
                    Image(nsImage: image).resizable().scaledToFill().frame(width: g.size.width, height: g.size.height).clipped()
                } else {
                    let (a, b) = Self.colors(for: seed)
                    LinearGradient(colors: [a, b], startPoint: .topLeading, endPoint: .bottomTrailing)
                    Circle().fill(.white.opacity(0.10)).frame(width: g.size.width * 1.1).offset(x: g.size.width * 0.35, y: -g.size.height * 0.35)
                    VStack(spacing: g.size.width * 0.03) {
                        Text(Self.monogram(name)).font(.system(size: g.size.width * 0.34, weight: .heavy, design: .rounded)).foregroundStyle(.white.opacity(0.95))
                        if showsName && g.size.width > 90 {
                            Text(name).font(.system(size: max(10, g.size.width * 0.075), weight: .semibold)).foregroundStyle(.white.opacity(0.85))
                                .lineLimit(2).multilineTextAlignment(.center).minimumScaleFactor(0.7).padding(.horizontal, g.size.width * 0.08)
                        }
                    }
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .clipShape(.rect(cornerRadius: radius, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(.white.opacity(0.10)))
        .accessibilityHidden(true)
    }

    /// Up to two capital letters: the initials of the first two words ("Super Mario Odyssey" -> "SM"), else the first two letters.
    static func monogram(_ name: String) -> String {
        let words = name.split { !$0.isLetter && !$0.isNumber }.filter { $0.first?.isLetter == true || $0.first?.isNumber == true }
        if words.count >= 2 { return String(words.prefix(2).compactMap(\.first)).uppercased() }
        return String(name.filter { $0.isLetter || $0.isNumber }.prefix(2)).uppercased()
    }

    /// Stable colours (white text passes 4.5:1 on both ends).
    static func colors(for seed: String) -> (Color, Color) {
        let palette: [(Color, Color)] = [
            (Color(red: 0.80, green: 0.20, blue: 0.22), Color(red: 0.55, green: 0.10, blue: 0.30)),
            (Color(red: 0.12, green: 0.50, blue: 0.62), Color(red: 0.10, green: 0.27, blue: 0.50)),
            (Color(red: 0.16, green: 0.50, blue: 0.36), Color(red: 0.08, green: 0.32, blue: 0.34)),
            (Color(red: 0.42, green: 0.33, blue: 0.70), Color(red: 0.25, green: 0.20, blue: 0.52)),
            (Color(red: 0.62, green: 0.28, blue: 0.72), Color(red: 0.40, green: 0.14, blue: 0.55)),
            (Color(red: 0.70, green: 0.40, blue: 0.10), Color(red: 0.55, green: 0.22, blue: 0.12)),
        ]
        let h = seed.unicodeScalars.reduce(5381) { ($0 &* 33) &+ Int($1.value) }
        return palette[abs(h) % palette.count]
    }
}

/// A progress ring with the percentage inside, for covers that are being built.
struct ProgressRing: View {
    var fraction: Double
    var lineWidth: CGFloat = 5
    var body: some View {
        ZStack {
            Circle().stroke(.white.opacity(0.25), lineWidth: lineWidth)
            Circle().trim(from: 0, to: max(0.02, fraction)).stroke(.white, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round)).rotationEffect(.degrees(-90))
                .animation(.smooth(duration: 0.3), value: fraction)
            Text("\(Int(fraction * 100))%").font(.system(size: 14, weight: .bold, design: .rounded)).foregroundStyle(.white).monospacedDigit()
        }
        .accessibilityElement().accessibilityLabel(Text("Progress")).accessibilityValue(Text("\(Int(fraction * 100)) percent"))
    }
}

/// The app's mark, drawn (the same two Joy-Con halves as the icon) so it is crisp at any size and needs no image file.
struct AppMark: View {
    var size: CGFloat = 80
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.225, style: .continuous)
                .fill(LinearGradient(colors: [Color(red: 0.98, green: 0.36, blue: 0.38), Theme.brandDeep], startPoint: .topLeading, endPoint: .bottomTrailing))
            HStack(spacing: size * 0.012) {
                ZStack {
                    Capsule().fill(Color(red: 0.09, green: 0.09, blue: 0.10)).frame(width: size * 0.27, height: size * 0.5)
                    Circle().fill(.white).frame(width: size * 0.11)
                }
                ZStack {
                    Capsule().fill(.white).frame(width: size * 0.27, height: size * 0.5)
                    Circle().fill(Color(red: 0.09, green: 0.09, blue: 0.10)).frame(width: size * 0.055).offset(y: -size * 0.075)
                    Circle().fill(Color(red: 0.09, green: 0.09, blue: 0.10)).frame(width: size * 0.055).offset(y: size * 0.075)
                    Circle().fill(Color(red: 0.09, green: 0.09, blue: 0.10)).frame(width: size * 0.055).offset(x: -size * 0.075)
                    Circle().fill(Color(red: 0.09, green: 0.09, blue: 0.10)).frame(width: size * 0.055).offset(x: size * 0.075)
                }
            }
        }
        .frame(width: size, height: size)
        .shadow(color: Theme.brand.opacity(0.35), radius: size * 0.14, y: size * 0.06)
        .accessibilityHidden(true)
    }
}
