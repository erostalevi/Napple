import Testing
import Foundation
@testable import NappleStudioCore

private func gdTmp() -> URL {
    let d = FileManager.default.temporaryDirectory.appendingPathComponent("gd-\(UUID().uuidString)")
    try! FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
    return d
}

private func entry(_ name: String, tid: String, built: TimeInterval, bytes: UInt64?) -> LibraryEntry {
    LibraryEntry(titleId: tid, name: name, version: "1.0.0", builtAt: Date(timeIntervalSince1970: built), bytes: bytes, path: "/x/\(name).app", bookmark: nil)
}

@Suite struct LibrarySortTests {
    let games = [entry("Zelda", tid: "0100000000000003", built: 100, bytes: 10),
                 entry("animal Crossing", tid: "0100000000000001", built: 300, bytes: 30),
                 entry("Mario 10", tid: "0100000000000002", built: 200, bytes: nil),
                 entry("Mario 2", tid: "0100000000000004", built: 200, bytes: 5)]

    @Test func recentIsNewestFirstAndTiesFallBackToName() {
        #expect(LibrarySort.recent.sorted(games).map(\.name) == ["animal Crossing", "Mario 2", "Mario 10", "Zelda"])
    }
    @Test func nameIsNaturalAndCaseInsensitive() {
        #expect(LibrarySort.name.sorted(games).map(\.name) == ["animal Crossing", "Mario 2", "Mario 10", "Zelda"])
    }
    @Test func sizeIsLargestFirstWithMissingSizesLast() {
        #expect(LibrarySort.size.sorted(games).map(\.name) == ["animal Crossing", "Zelda", "Mario 2", "Mario 10"])
    }
    @Test func titleIdIsAscending() {
        #expect(LibrarySort.titleId.sorted(games).map(\.titleId) == ["0100000000000001", "0100000000000002", "0100000000000003", "0100000000000004"])
    }
    @Test func sortingIsStableAcrossShuffles() {
        for s in LibrarySort.allCases { #expect(s.sorted(games) == s.sorted(games.reversed())) }
    }
    @Test func searchMatchesEveryWordInNameOrTitleId() {
        let g = entry("Pokémon Sword", tid: "0100ABF008968000", built: 0, bytes: nil)
        #expect(g.matches(query: ""))
        #expect(g.matches(query: "pokemon"))
        #expect(g.matches(query: "sword poke"))
        #expect(g.matches(query: "abf008"))
        #expect(!g.matches(query: "shield"))
        #expect(!g.matches(query: "sword shield"))
    }
}

@Suite struct GameDetailsTests {
    private func makeApp(_ root: URL, manifest: String?) -> URL {
        let app = root.appendingPathComponent("G.app"), fm = FileManager.default
        let game = app.appendingPathComponent("Contents/Resources/game")
        for v in ["1.0.0", "2.0.0"] { try! fm.createDirectory(at: game.appendingPathComponent(v), withIntermediateDirectories: true) }
        try! "2.0.0\n".write(to: game.appendingPathComponent("default"), atomically: true, encoding: .utf8)
        if let manifest { try! manifest.write(to: game.appendingPathComponent("2.0.0/manifest.json"), atomically: true, encoding: .utf8) }
        return app
    }

    @Test func readsVersionsModsAndSaves() throws {
        let root = gdTmp(), support = gdTmp()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: support) }
        let app = makeApp(root, manifest: #"{"mods":[{"id":"fps60","version":"1.2"},{"id":"","version":"x"},{"id":"hud"}]}"#)
        let data = GameDetails.dataFolderPath(titleId: "0100aa", support: support)
        try FileManager.default.createDirectory(at: data.appendingPathComponent("save/space1"), withIntermediateDirectories: true)
        try "{}".write(to: data.appendingPathComponent("settings.json"), atomically: true, encoding: .utf8)
        try Data(repeating: 1, count: 5000).write(to: data.appendingPathComponent("save/space1/a.bin"))
        let d = GameDetails.read(app: app, titleId: "0100AA", support: support)
        #expect(d.embeddedVersions == ["2.0.0", "1.0.0"])
        #expect(d.mods == [AppliedMod(id: "fps60", version: "1.2"), AppliedMod(id: "hud", version: "")])
        #expect(d.dataFolder?.lastPathComponent == "0100AA" && d.saveFolder?.lastPathComponent == "save")
        #expect(d.saveBytes >= 5000 && d.hasCustomSettings)
    }

    @Test func aGameThatNeverRanHasNoFolders() {
        let root = gdTmp(), support = gdTmp()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: support) }
        let d = GameDetails.read(app: makeApp(root, manifest: nil), titleId: "0100BB", support: support)
        #expect(d.dataFolder == nil && d.saveFolder == nil && d.mods.isEmpty && d.saveBytes == 0)
    }

    @Test func aMissingAppIsEmptyNotACrash() {
        let d = GameDetails.read(app: URL(fileURLWithPath: "/nonexistent/x.app"), titleId: "0100CC", support: gdTmp())
        #expect(d == GameDetails())
    }
}

@Suite struct ModPickerTests {
    @Test func acceptsNapmodFilesAndModFoldersOnly() throws {
        let d = gdTmp(); defer { try? FileManager.default.removeItem(at: d) }
        let pack = d.appendingPathComponent("a.NAPMOD"); try Data().write(to: pack)
        let dir = d.appendingPathComponent("folder"); try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try "{}".write(to: dir.appendingPathComponent("mod.json"), atomically: true, encoding: .utf8)
        let empty = d.appendingPathComponent("empty"); try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        let txt = d.appendingPathComponent("notes.txt"); try Data().write(to: txt)
        let r = ModPicker.filter([pack, dir, empty, txt, pack, d.appendingPathComponent("gone.napmod")])
        #expect(r.accepted == [pack, dir])
        #expect(r.rejected == [empty, txt, d.appendingPathComponent("gone.napmod")])
        #expect(ModPicker.filter([pack], existing: [pack]).accepted.isEmpty)
    }

    @Test func buildArgumentsRepeatTheFlagPerMod() {
        let a = [URL(fileURLWithPath: "/m/a,b.napmod"), URL(fileURLWithPath: "/m/c")]
        #expect(NappleCLI.modsArguments(a) == ["--mods", "/m/a,b.napmod", "--mods", "/m/c"])
        #expect(NappleCLI.modsArguments([]).isEmpty)
    }
}

@Suite struct AppVersionTests {
    @Test func versionStringCarriesTheBetaMark() {
        #expect(AppVersion.display(version: "0.2.0", build: "812") == "0.2.0 Beta (812)")
        #expect(AppVersion.display(version: nil, build: nil) == "0.1.0 Beta")
        #expect(AppVersion.display(version: "1.0", build: "", beta: false) == "1.0")
    }
}
