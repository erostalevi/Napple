import SwiftUI
import AppKit

/// The About window: mark, name, version with the Beta mark, what the app is, and the personal-use reminder.
struct AboutView: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        VStack(spacing: 14) {
            AppMark(size: 96).padding(.top, 8)
            VStack(spacing: 6) {
                HStack(spacing: 8) { Text("Napple Studio").font(.system(size: 24, weight: .bold)); BetaBadge() }
                Text(AppModel.versionString).font(.callout).foregroundStyle(.secondary).textSelection(.enabled).accessibilityIdentifier("about.version")
            }
            Text("Turns your own Switch game dumps into native apps for Mac and iPhone. Everything happens on this Mac: no account, no server, no tracking.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            VStack(spacing: 4) {
                if let e = m.engineSummary { Text(e).font(.caption).foregroundStyle(.tertiary) }
                Text("For personal use with games and keys you own. Do not share built apps, keys or firmware.").font(.caption).foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 10) {
                Link(destination: URL(string: "https://napple.app/guides/studio")!) { Label("Guide", systemImage: "book") }
                Button { m.showLogsInFinder() } label: { Label("Logs", systemImage: "doc.text.magnifyingglass") }
            }
            .buttonStyle(.glass).padding(.top, 2)
        }
        .padding(28).frame(width: 380)
        .background(Backdrop())
        .tint(Theme.brand)
        .fixedSize()
    }
}
