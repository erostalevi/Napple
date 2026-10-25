import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// Open panel helper (files and/or folders). Returns nothing when cancelled.
func pick(files: Bool, dirs: Bool, multiple: Bool = false, message: String, types: [UTType] = []) -> [URL] {
    let p = NSOpenPanel()
    p.canChooseFiles = files; p.canChooseDirectories = dirs; p.allowsMultipleSelection = multiple
    p.message = message
    if !types.isEmpty { p.allowedContentTypes = types }
    return p.runModal() == .OK ? p.urls : []
}

// MARK: palette (the app icon's red, pink and violet over dark glass)

enum Palette {
    static let red = Color(red: 1.0, green: 0.369, blue: 0.384)       // FF5E62
    static let pink = Color(red: 1.0, green: 0.541, blue: 0.561)      // FF8A8F
    static let violet = Color(red: 0.486, green: 0.361, blue: 1.0)    // 7C5CFF
    static let amber = Color(red: 1.0, green: 0.816, blue: 0.541)     // FFD08A
    static let mint = Color(red: 0.482, green: 0.878, blue: 0.627)    // 7BE0A0
    static let danger = Color(red: 1.0, green: 0.541, blue: 0.561)
    static let ink = Color(red: 0.071, green: 0.047, blue: 0.102)     // text on the red gradient
    static let secondary = Color.white.opacity(0.74)
    static let tertiary = Color.white.opacity(0.6)
    static let fill = Color.white.opacity(0.07)
    static let fillStrong = Color.white.opacity(0.11)
    static let stroke = Color.white.opacity(0.14)
    static let strokeStrong = Color.white.opacity(0.26)
    static let gradient = LinearGradient(colors: [red, pink], startPoint: .topLeading, endPoint: .bottomTrailing)
    static let progress = LinearGradient(colors: [red, violet], startPoint: .leading, endPoint: .trailing)
}

/// Rounded translucent panel. Inner radius = outer radius - padding where panels nest (28 / 14, 20 / 12).
struct Card: ViewModifier {
    var radius: CGFloat = 20
    var padding: CGFloat = 12
    var tint: Color? = nil
    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        content
            .padding(padding)
            .background(tint ?? Palette.fill, in: shape)
            .overlay(shape.strokeBorder(Palette.stroke))
    }
}
extension View {
    func card(radius: CGFloat = 20, padding: CGFloat = 12, tint: Color? = nil) -> some View {
        modifier(Card(radius: radius, padding: padding, tint: tint))
    }
}

// MARK: buttons

struct PrimaryPill: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    var height: CGFloat = 40
    var fontSize: CGFloat = 14
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: fontSize, weight: .semibold))
            .foregroundStyle(Palette.ink)
            .padding(.horizontal, 20)
            .frame(height: height)
            .background(Palette.gradient, in: Capsule())
            .opacity(enabled ? (configuration.isPressed ? 0.82 : 1) : 0.4)
            .contentShape(Capsule())
    }
}

struct SecondaryPill: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    var height: CGFloat = 32
    var fontSize: CGFloat = 12
    var selected = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: fontSize, weight: .medium))
            .foregroundStyle(Color.white.opacity(0.92))
            .padding(.horizontal, 12)
            .frame(height: height)
            .background(selected ? Palette.fillStrong : Palette.fill, in: Capsule())
            .overlay(Capsule().strokeBorder(Palette.strokeStrong))
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.4)
            .contentShape(Capsule())
    }
}

struct CircleIcon: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    var size: CGFloat = 28
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: size * 0.46, weight: .semibold))
            .foregroundStyle(Color.white.opacity(0.92))
            .frame(width: size, height: size)
            .background(Palette.fill, in: Circle())
            .overlay(Circle().strokeBorder(Palette.strokeStrong))
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.4)
            .contentShape(Circle())
    }
}

struct SectionLabel: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text.uppercased())
            .font(.system(size: 11, weight: .semibold))
            .tracking(0.8)
            .foregroundStyle(Palette.tertiary)
            .accessibilityAddTraits(.isHeader)
    }
}

// MARK: icons and badges

/// The icon of a built app (what Finder shows), or a game-controller tile when there is none (yet).
struct AppIconView: View {
    let url: URL?
    var size: CGFloat = 72
    var radius: CGFloat = 14
    @State private var image: NSImage?
    var body: some View {
        Group {
            if let image {
                Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
            } else {
                GameTile(size: size, radius: radius)
            }
        }
        .frame(width: size, height: size)
        .task(id: url?.path) {
            guard let u = url, FileManager.default.fileExists(atPath: u.path) else { image = nil; return }
            image = NSWorkspace.shared.icon(forFile: u.path)
        }
        .accessibilityHidden(true)
    }
}

/// Stand-in for a game's own icon, which only exists once the app is built.
struct GameTile: View {
    var size: CGFloat = 72
    var radius: CGFloat = 14
    var body: some View {
        RoundedRectangle(cornerRadius: radius, style: .continuous)
            .fill(Palette.fillStrong)
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Palette.strokeStrong, style: StrokeStyle(lineWidth: 1, dash: [4, 3])))
            .overlay(Image(systemName: "gamecontroller").font(.system(size: size * 0.36)).foregroundStyle(Palette.secondary))
            .frame(width: size, height: size)
            .accessibilityHidden(true)
    }
}

enum StatusTone { case good, warn, bad, neutral
    var color: Color {
        switch self { case .good: return Palette.mint; case .warn: return Palette.amber; case .bad: return Palette.danger; case .neutral: return Palette.tertiary }
    }
}

extension CompatStatus {
    var tone: StatusTone {
        switch self {
        case .perfect, .playable: return .good
        case .partial, .menus, .boots: return .warn
        case .nothing: return .bad
        case .unknown: return .neutral
        }
    }
    var symbol: String {
        switch self {
        case .perfect, .playable: return "checkmark.circle.fill"
        case .partial, .menus, .boots: return "circle.lefthalf.filled"
        case .nothing: return "xmark.circle.fill"
        case .unknown: return "questionmark.circle"
        }
    }
}

/// "Partial", "Playable", ... from the compatibility matrix; "Not tested yet" when the game is not in it.
struct CompatBadge: View {
    let entry: CompatEntry?
    var body: some View {
        let status = entry?.status ?? .unknown
        let text = entry == nil ? "Not tested yet" : status.label
        Label(text, systemImage: status.symbol)
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(entry == nil ? StatusTone.neutral.color : status.tone.color)
            .labelStyle(.titleAndIcon)
            .accessibilityLabel("Compatibility: \(text)")
    }
}

func stageTone(_ status: String) -> StatusTone {
    switch status.lowercased() {
    case "pass": return .good
    case "partial": return .warn
    case "fail", "blocked": return .bad
    default: return .neutral
    }
}

func stageSymbol(_ status: String) -> String {
    switch status.lowercased() {
    case "pass": return "checkmark"
    case "partial": return "circle.lefthalf.filled"
    case "fail": return "xmark.circle.fill"
    case "blocked": return "minus.circle.fill"
    default: return "circle.dashed"
    }
}

// MARK: footer, toast, banners

struct LegalFooter: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 4) {
            if let msg = m.engineMessage {
                Text(msg).font(.system(size: 12)).foregroundStyle(Palette.secondary).multilineTextAlignment(.center)
            }
            Text("Built apps contain game-derived content for your personal use only. Do not share them.")
                .font(.system(size: 12)).foregroundStyle(Palette.tertiary).multilineTextAlignment(.center)
                .accessibilityLabel("Legal notice: built apps are for your personal use only and must not be shared")
        }
        .frame(maxWidth: .infinity)
    }
}

struct ToastView: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(.white)
            .multilineTextAlignment(.center)
            .padding(.horizontal, 16).padding(.vertical, 10)
            .background(.regularMaterial, in: Capsule())
            .overlay(Capsule().strokeBorder(Palette.strokeStrong))
            .frame(maxWidth: 640)
            .accessibilityAddTraits(.updatesFrequently)
    }
}

/// A one-message banner: error (red), caution (amber) or info.
struct Banner: View {
    let text: String
    var tone: StatusTone = .bad
    var symbol: String? = nil
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: symbol ?? (tone == .bad ? "exclamationmark.triangle.fill" : tone == .warn ? "exclamationmark.triangle" : "info.circle"))
                .foregroundStyle(tone.color)
            Text(text).font(.system(size: 12)).foregroundStyle(Palette.secondary)
                .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(tone == .bad ? "Error" : "Note"): \(text)")
    }
}

/// Heading + subtitle used at the top of most screens.
struct ScreenTitle: View {
    let title: String
    var subtitle: String? = nil
    var size: CGFloat = 32
    var centered = false
    var body: some View {
        VStack(alignment: centered ? .center : .leading, spacing: 4) {
            Text(title).font(.system(size: size, weight: .medium, design: .serif)).foregroundStyle(.white)
                .multilineTextAlignment(centered ? .center : .leading)
                .accessibilityAddTraits(.isHeader)
            if let subtitle {
                Text(subtitle).font(.system(size: 14)).foregroundStyle(Palette.secondary)
                    .multilineTextAlignment(centered ? .center : .leading).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
