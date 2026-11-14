import SwiftUI
import AppKit
import NappleStudioCore

/// `NAPPLE_TOUR_DIR=<dir>` (or `--debug-tour <dir>`; the environment is safer, AppKit sometimes takes a path argument for a file to open and then shows no window): visits every pane with fake data, and for each one writes `<dir>/<name>.rect` ("x,y,w,h", the window's top-left screen rect)
/// and waits (up to 25 s) for `<dir>/<name>.png`, so a script can run `screencapture -x -R "$(cat name.rect)" name.png`. `--debug-light` / `--debug-dark`
/// pick the appearance. Run it with `--support-dir` and `--output-dir` pointing at scratch folders: it creates fake keys there and never touches your library.
/// (CGWindowList is blind in the sandboxed shell and NSView.cacheDisplay renders materials blank, which is why the screenshot is taken from outside.)
@MainActor
enum DebugTour {
    static func arg(_ n: String) -> String? {
        let a = CommandLine.arguments
        return a.firstIndex(of: n).flatMap { $0 + 1 < a.count ? a[$0 + 1] : nil }
    }

    static func startIfRequested(_ m: AppModel) {
        guard let dir = ProcessInfo.processInfo.environment["NAPPLE_TOUR_DIR"] ?? arg("--debug-tour") else { return }
        let url = URL(fileURLWithPath: dir, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        Task { await run(m, dir: url) }
    }

    private static func rect(of w: NSWindow) -> String {
        let top = NSScreen.screens.first?.frame.maxY ?? 0
        let f = w.frame
        return "\(Int(f.minX)),\(Int(top - f.maxY)),\(Int(f.width)),\(Int(f.height))"
    }

    private static func snap(_ name: String, window: NSWindow?, dir: URL) async {
        guard let w = window else { FileHandle.standardError.write(Data("tour: no window for \(name)\n".utf8)); return }
        if w.frame.minY < 0 || (NSScreen.screens.first.map { !$0.frame.intersects(w.frame) } ?? false) { placeOnMainScreen(w) }
        NSApp.activate()
        w.level = .floating   // above every other window, so the captured rect holds this window and nothing else
        w.makeKeyAndOrderFront(nil)
        try? await Task.sleep(nanoseconds: 900_000_000)
        try? rect(of: w).write(to: dir.appendingPathComponent("\(name).rect"), atomically: true, encoding: .utf8)
        let png = dir.appendingPathComponent("\(name).png")
        try? FileManager.default.removeItem(at: png)
        for _ in 0..<250 {
            if FileManager.default.fileExists(atPath: png.path) { break }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        try? await Task.sleep(nanoseconds: 200_000_000)
    }

    /// Top-left of the built-in (primary) display, so the screenshot rect is always on the same screen.
    private static func placeOnMainScreen(_ w: NSWindow?) {
        guard let w, let s = NSScreen.screens.first else { return }
        w.setFrameTopLeftPoint(NSPoint(x: s.visibleFrame.minX + 60, y: s.visibleFrame.maxY - 30))
    }

    private static func settingsWindow() -> NSWindow? {
        NSApp.windows.first { $0.isVisible && $0.styleMask.contains(.titled) && $0.title != "Napple Studio" && $0.title != "About Napple Studio" && !$0.title.hasPrefix("Console") }
    }

    static func run(_ m: AppModel, dir: URL) async {
        try? await Task.sleep(nanoseconds: 1_200_000_000)
        NSApp.setActivationPolicy(.regular)   // a bare executable (no .app) starts as an accessory and its windows stay behind other apps
        NSApp.activate()
        if CommandLine.arguments.contains("--debug-light") { NSApp.appearance = NSAppearance(named: .aqua) }
        if CommandLine.arguments.contains("--debug-dark") { NSApp.appearance = NSAppearance(named: .darkAqua) }
        let main = m.window
        main?.setContentSize(Theme.windowSize)
        placeOnMainScreen(main)

        // 1. first run (no keys yet)
        m.onboardingDone = false
        for (i, n) in ["welcome-keys", "welcome-engine", "welcome-game"].enumerated() {
            m.onboardingStep = i
            await snap(n, window: main, dir: dir)
        }

        // fake keys so the rest of the app is usable
        let names = KeysetManager.requiredKeyNames + ["header_key"]
        let keys = dir.appendingPathComponent("prod.keys")
        try? names.map { "\($0) = 00112233445566778899aabbccddeeff" }.joined(separator: "\n").write(to: keys, atomically: true, encoding: .utf8)
        if let set = try? m.keysets.save(name: "Tour", keys: keys, titleKeys: nil, firmware: nil, replace: true) { m.keySetCreated(set); m.keysChanged() }
        m.onboardingDone = true
        m.refreshLibrary(adoptOutput: true)
        m.sort = .name
        m.libraryItems = [("Metroid Dread", "010093801237C000", "2.1.0"), ("Sample Racer 8", "0100152000022000", "3.0.1"),
                          ("Farm Island", "01006F8002326000", "2.0.2"), ("Odyssey Quest", "0100000000010000", "1.3.0")].map { n in
            LibraryItem(entry: LibraryEntry(titleId: n.1, name: n.0, version: n.2, builtAt: Date().addingTimeInterval(-86_400), bytes: 4_700_000_000,
                                            path: "/tmp/napple-tour/\(n.0).app", bookmark: nil), status: .present, compat: nil)
        }
        m.resetDebugState()

        // 2. library
        m.viewMode = .grid
        await snap("library-grid", window: main, dir: dir)
        m.applyDebugState("selected")
        await snap("library-selected", window: main, dir: dir)
        m.resetDebugState()
        m.viewMode = .list
        await snap("library-list", window: main, dir: dir)
        m.applyDebugState("selected")
        await snap("library-list-selected", window: main, dir: dir)
        m.resetDebugState(); m.viewMode = .grid
        m.searchText = "zzz"
        await snap("library-no-results", window: main, dir: dir)
        m.resetDebugState()

        // 3. add / build flow
        for n in ["reading", "add", "add-mods", "blockers", "add-ios", "building", "done", "failed"] {
            m.resetDebugState(); m.applyDebugState(n)
            await snap("flow-\(n)", window: main, dir: dir)
        }
        m.resetDebugState(); m.applyDebugState("building"); m.panelOpen = false
        await snap("library-building", window: main, dir: dir)
        m.resetDebugState(); m.applyDebugState("drop")
        await snap("drop-overlay", window: main, dir: dir)
        m.resetDebugState(); m.applyDebugState("legal")
        await snap("legal", window: main, dir: dir)
        m.resetDebugState()

        // 4. narrow window
        main?.setContentSize(Theme.minWindowSize)
        m.applyDebugState("selected")
        await snap("library-min-size", window: main, dir: dir)
        m.resetDebugState()
        main?.setContentSize(Theme.windowSize)

        // 5. empty library
        let saved = m.libraryItems
        m.libraryItems = []
        await snap("library-empty", window: main, dir: dir)
        m.libraryItems = saved

        // 6. console and about
        let c = DebugConsole(name: "Metroid Dread", app: URL(fileURLWithPath: "/x/Metroid Dread.app"))
        c.debugFill(["napple-runtime 0.1.0 (beta)", "[boot] title 010093801237C000 version 2.1.0", "[gfx] Metal device: Apple M4", "[nvn] 214 functions bound",
                     "warning: 3 shaders use an unsupported opcode, replay path enabled", "[audio] 48000 Hz stereo (silent for this tour)", "error: svc 0x7C returned 0xE401 (ignored)", "[frame] 60.0 fps"])
        m.consoles.append(c); m.consoleToOpen = c.id
        try? await Task.sleep(nanoseconds: 1_500_000_000)
        await snap("console", window: NSApp.windows.first { $0.title.hasPrefix("Console") }, dir: dir)
        NSApp.windows.first { $0.title.hasPrefix("Console") }?.close()
        await openAbout(dir: dir)

        // 7. settings tabs
        m.openSettings(tab: "general")
        try? await Task.sleep(nanoseconds: 1_200_000_000)
        for tab in SettingsRoot.tabs {
            m.settingsTab = tab
            await snap("settings-\(tab)", window: settingsWindow(), dir: dir)
        }
        try? "done".write(to: dir.appendingPathComponent("DONE"), atomically: true, encoding: .utf8)
        if CommandLine.arguments.contains("--debug-tour-quit") { exit(0) }
    }

    private static func openAbout(dir: URL) async {
        // the About window is opened through the app menu item so no SwiftUI environment is needed here
        if let menu = NSApp.mainMenu?.items.first?.submenu, let i = menu.items.firstIndex(where: { $0.title.hasPrefix("About") }) { menu.performActionForItem(at: i) }
        try? await Task.sleep(nanoseconds: 1_200_000_000)
        let w = NSApp.windows.first { $0.isVisible && $0.title == "About Napple Studio" }
        await snap("about", window: w, dir: dir)
        w?.close()
    }
}
