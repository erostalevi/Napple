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
                .background(.regularMaterial, in: .rect(cornerRadius: 16, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).strokeBorder(Theme.hairline))
                .shadow(color: .black.opacity(0.4), radius: 28, y: 10)
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

/// First launch: keys and firmware are required before anything else, and there is no way around it.
struct OnboardingDialog: View {
    @Environment(AppModel.self) private var m
    @State private var keys: URL?
    @State private var titleKeys: URL?
    @State private var firmware: URL?
    @State private var scanNote: String?
    @State private var error: String?
    @State private var busy = false

    var body: some View {
        DialogCard(width: 540) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Welcome to Napple Studio").font(.system(size: 22, weight: .semibold))
                Text("Add your keys and firmware once. Then build as many games as you like. This step is required.").foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Choose the folder").fontWeight(.semibold)
                    Text("Napple finds prod.keys, title.keys and the firmware inside it.").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Choose Folder…") { chooseFolder() }.buttonStyle(.glassProminent).accessibilityIdentifier("onboarding.folder")
            }
            .padding(12).background(Theme.surface, in: .rect(cornerRadius: Theme.cardRadius, style: .continuous))
            RowGroup {
                fileRow("prod.keys", url: keys, required: true) { keys = pick(files: true, dirs: false, message: String(localized: "Choose prod.keys")).first }
                fileRow("title.keys", url: titleKeys, required: false) { titleKeys = pick(files: true, dirs: false, message: String(localized: "Choose title.keys")).first }
                fileRow(String(localized: "Firmware"), url: firmware, required: false, divider: false) { firmware = pick(files: true, dirs: true, message: String(localized: "Choose the firmware zip or folder")).first }
            }
            if let scanNote { Text(scanNote).font(.caption).foregroundStyle(keys == nil ? Theme.warn : .secondary) }
            if let error { Notice(text: error, tone: .bad) }
            Text("Files are copied to Napple's private folder, owner-only. Key values are never shown or logged.").font(.caption).foregroundStyle(.secondary)
            HStack {
                Spacer()
                Button(busy ? "Copying…" : "Save and Continue") { save() }
                    .buttonStyle(.glassProminent).controlSize(.large).disabled(busy || keys == nil).keyboardShortcut(.defaultAction).accessibilityIdentifier("onboarding.save")
            }
        }
    }

    private func fileRow(_ label: String, url: URL?, required: Bool, divider: Bool = true, choose: @escaping () -> Void) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Text(verbatim: label)
                Spacer()
                if let url { Text(url.lastPathComponent).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle); Image(systemName: "checkmark").foregroundStyle(Theme.good) }
                else { Text(required ? "Required" : "Optional").foregroundStyle(.secondary) }
                Button(url == nil ? "Choose…" : "Change…", action: choose).buttonStyle(.glass).controlSize(.small)
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            if divider { Divider().overlay(Theme.hairline).padding(.leading, 12) }
        }
    }

    private func chooseFolder() {
        guard let f = pick(files: false, dirs: true, message: String(localized: "Choose the folder with your keys and firmware")).first else { return }
        let s = KeyFinder.scan(f)
        keys = s.keys ?? keys; titleKeys = s.titleKeys ?? titleKeys; firmware = s.firmware ?? firmware
        var found: [String] = []
        if s.keys != nil { found.append("prod.keys") }
        if s.titleKeys != nil { found.append("title.keys") }
        if s.firmware != nil { found.append(String(localized: "firmware")) }
        scanNote = found.isEmpty ? String(localized: "Nothing recognised in that folder. Choose each file below.") : String(localized: "Found: \(found.joined(separator: ", "))")
    }

    private func save() {
        busy = true; error = nil
        let (k, t, f) = (keys, titleKeys, firmware)
        let mgr = m.keysets
        Task.detached {
            let res = Result { try mgr.save(name: "Default", keys: k, titleKeys: t, firmware: f, replace: true) }
            await MainActor.run {
                busy = false
                switch res {
                case .success(let set): m.keysChanged(); m.selectedSetName = set.name
                case .failure(let e): error = e.localizedDescription
                }
            }
        }
    }
}

/// Over the whole window while files are dragged in.
struct DropOverlay: View {
    var body: some View {
        ZStack {
            Rectangle().fill(.ultraThinMaterial)
            RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(Color.accentColor, style: StrokeStyle(lineWidth: 2, dash: [10, 8])).padding(16)
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
                .glassEffect(.regular, in: .capsule)
                .padding(.bottom, 20)
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .accessibilityAddTraits(.updatesFrequently)
        }
    }
}
