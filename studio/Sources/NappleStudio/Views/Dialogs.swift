import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NappleStudioCore

/// A centred card over a dimmed window.
struct DialogCard<Content: View>: View {
    var width: CGFloat = 440
    @ViewBuilder var content: Content
    var body: some View {
        ZStack {
            Rectangle().fill(.black.opacity(0.42)).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) { content }
                .padding(26).frame(width: width)
                .glassSurface(radius: 20)
                .shadow(color: .black.opacity(0.35), radius: 28, y: 10)
        }
        .transition(.opacity)
        .accessibilityAddTraits(.isModal)
    }
}

/// Shown once, before the first build.
struct LegalDialog: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        DialogCard {
            Label { Text("Personal use only").font(.title2.weight(.semibold)) } icon: { Image(systemName: "hand.raised").foregroundStyle(.tint) }
            Text("Built apps contain content derived from your own game files. Keep them for yourself and never share them, your keys or your firmware.")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("Cancel") { m.showLegal = false }.controlSize(.large).keyboardShortcut(.cancelAction)
                Button("I Understand") { m.legalAck = true; m.showLegal = false; m.startBuild() }
                    .buttonStyle(.glassProminent).controlSize(.large).keyboardShortcut(.defaultAction).accessibilityIdentifier("legal.ok")
            }
            Text("Shown once, before your first build.").font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .trailing)
        }
    }
}

/// Over the whole window while files are dragged in.
struct DropOverlay: View {
    var body: some View {
        ZStack {
            Rectangle().fill(.ultraThinMaterial)
            RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(Theme.brand, style: StrokeStyle(lineWidth: 2, dash: [10, 8])).padding(16)
            VStack(spacing: 12) {
                Image(systemName: "arrow.down.to.line").font(.system(size: 52, weight: .ultraLight)).foregroundStyle(.tint)
                Text("Drop to add").font(.system(size: 22, weight: .semibold))
                Text("NSP, XCI, NSZ or XCZ files, or folders holding them.").foregroundStyle(.secondary)
            }
        }
        .allowsHitTesting(false)
        .transition(.opacity)
    }
}

struct ToastHost: View {
    @Environment(AppModel.self) private var m
    var body: some View {
        if let t = m.toast {
            Text(t).font(.callout).padding(.horizontal, 16).padding(.vertical, 10)
                .glassCapsule()
                .padding(.bottom, 20)
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .accessibilityAddTraits(.updatesFrequently)
        }
    }
}
