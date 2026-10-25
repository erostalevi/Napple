import Foundation
import Testing
@testable import NappleStudioCore

/// A folder shaped like a built Napple Mac app (`Contents/Info.plist` with `NappleTitleId`).
@discardableResult
func fakeApp(in dir: URL, name: String = "Game", titleId: String = "0100000000010000", version: String = "1.0.0", mac: Bool = true) -> URL {
    let app = dir.appendingPathComponent("\(name).app", isDirectory: true)
    let plistDir = mac ? app.appendingPathComponent("Contents", isDirectory: true) : app
    try! FileManager.default.createDirectory(at: plistDir, withIntermediateDirectories: true)
    let info: [String: Any] = [
        "CFBundleName": name, "CFBundleIdentifier": "com.napple.game.\(titleId.lowercased())",
        "CFBundleShortVersionString": version, "NappleTitleId": titleId,
    ]
    let data = try! PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
    try! data.write(to: plistDir.appendingPathComponent("Info.plist"))
    return app
}

@Suite struct InputExpanderTests {
    @Test func findsGameFilesInFoldersAndSkipsOthers() throws {
        let root = tmp(), fm = FileManager.default
        let sub = root.appendingPathComponent("games/deep"); try fm.createDirectory(at: sub, withIntermediateDirectories: true)
        for n in ["a.nsp", "b.XCI", "c.nsz", "d.xcz", "notes.txt"] { try Data([1]).write(to: root.appendingPathComponent("games/\(n)")) }
        try Data([1]).write(to: sub.appendingPathComponent("e.nsp"))
        try Data([1]).write(to: root.appendingPathComponent("games/.hidden.nsp"))
        let r = InputExpander.expand([root.appendingPathComponent("games")])
        #expect(Set(r.files.map(\.lastPathComponent)) == ["a.nsp", "b.XCI", "c.nsz", "d.xcz", "e.nsp"])
        #expect(r.ignored.isEmpty && r.unreadable.isEmpty && !r.truncated)
        #expect(r.skippedNote == nil)
    }

    @Test func reportsIgnoredMissingAndDuplicates() throws {
        let root = tmp()
        let game = root.appendingPathComponent("g.nsp"); try Data([1]).write(to: game)
        let pdf = root.appendingPathComponent("x.pdf"); try Data([1]).write(to: pdf)
        let empty = root.appendingPathComponent("empty"); try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        let gone = root.appendingPathComponent("gone.nsp")
        let r = InputExpander.expand([game, game, pdf, empty, gone])
        #expect(r.files.count == 1)                       // dropped twice, listed once
        #expect(r.ignored.map(\.lastPathComponent).sorted() == ["empty", "x.pdf"])
        #expect(r.unreadable == [gone])
        #expect(r.skippedNote?.contains("not a game file") == true)
    }

    @Test func emptyDropGivesNothing() {
        #expect(InputExpander.expand([]).files.isEmpty)
    }
}

@Suite struct BuiltAppInfoTests {
    @Test func readsMacAndFlatBundles() {
        let dir = tmp()
        let mac = BuiltAppInfo.read(fakeApp(in: dir, name: "Mario", titleId: "0100000000010000", version: "1.2.3"))
        #expect(mac?.name == "Mario" && mac?.version == "1.2.3" && mac?.isMac == true && mac?.titleId == "0100000000010000")
        let ios = BuiltAppInfo.read(fakeApp(in: dir, name: "Phone", mac: false))
        #expect(ios?.isMac == false)
        #expect(BuiltAppInfo.read(dir.appendingPathComponent("Nope.app")) == nil)
    }

    @Test func fallsBackToBundleIdAndRejectsOtherApps() throws {
        let dir = tmp(), fm = FileManager.default
        let app = dir.appendingPathComponent("Old.app/Contents"); try fm.createDirectory(at: app, withIntermediateDirectories: true)
        let old: [String: Any] = ["CFBundleIdentifier": "com.napple.game.01006bb00c6f0000", "CFBundleName": "Old"]
        try PropertyListSerialization.data(fromPropertyList: old, format: .xml, options: 0).write(to: app.appendingPathComponent("Info.plist"))
        #expect(BuiltAppInfo.read(dir.appendingPathComponent("Old.app"))?.titleId == "01006BB00C6F0000")
        let other = dir.appendingPathComponent("Other.app/Contents"); try fm.createDirectory(at: other, withIntermediateDirectories: true)
        try PropertyListSerialization.data(fromPropertyList: ["CFBundleIdentifier": "com.example.x"], format: .xml, options: 0)
            .write(to: other.appendingPathComponent("Info.plist"))
        #expect(BuiltAppInfo.read(dir.appendingPathComponent("Other.app")) == nil)
    }
}

@Suite struct LibraryStoreTests {
    func store() -> (LibraryStore, URL) {
        let root = tmp()
        return (LibraryStore(file: root.appendingPathComponent("library.json")), root)
    }

    @Test func addPersistsAndDedupesByPath() throws {
        let (s, root) = store()
        let out = tmp()
        let app = fakeApp(in: out)
        let info = BuiltAppInfo.read(app)!
        try s.add(app: app, info: info, bytes: 100)
        try s.add(app: app, info: info, bytes: 200)             // rebuilt into the same folder
        #expect(s.entries.count == 1 && s.entries[0].bytes == 200)
        let reopened = LibraryStore(file: root.appendingPathComponent("library.json"))
        #expect(reopened.entries.map(\.name) == ["Game"])
        let attrs = try FileManager.default.attributesOfItem(atPath: root.appendingPathComponent("library.json").path)
        #expect((attrs[.posixPermissions] as? Int) == 0o600)
    }

    @Test func iosAppsAreNotKept() {
        let (s, _) = store()
        let app = fakeApp(in: tmp(), mac: false)
        #expect(throws: LibraryError.notMacApp) { try s.add(app: app, info: BuiltAppInfo.read(app)!, bytes: nil) }
        #expect(s.entries.isEmpty)
    }

    @Test func followsAppsThatMove() throws {
        let (s, _) = store()
        let a = tmp(), b = tmp()
        let app = fakeApp(in: a)
        let e = try s.add(app: app, info: BuiltAppInfo.read(app)!, bytes: nil)
        let moved = b.appendingPathComponent("Renamed.app")
        try FileManager.default.moveItem(at: app, to: moved)
        let changes = s.refresh()
        #expect(changes.contains { $0.kind == .moved })
        #expect(s.status(of: e.id) == .present)
        #expect(s.url(of: e.id)?.standardizedFileURL.resolvingSymlinksInPath().path == moved.standardizedFileURL.resolvingSymlinksInPath().path)
    }

    @Test func missingThenRestored() throws {
        let (s, _) = store()
        let out = tmp()
        let app = fakeApp(in: out)
        let e = try s.add(app: app, info: BuiltAppInfo.read(app)!, bytes: nil)
        let hidden = out.appendingPathComponent("hold")
        try FileManager.default.createDirectory(at: hidden, withIntermediateDirectories: true)
        // delete the bundle outright: bookmark cannot find it either
        try FileManager.default.removeItem(at: app)
        _ = s.refresh()
        #expect(s.status(of: e.id) == .missing)
        #expect(s.entries.count == 1)                           // kept so the user can locate or remove it
        fakeApp(in: out)                                        // rebuilt at the same path
        let changes = s.refresh()
        #expect(s.status(of: e.id) == .present)
        #expect(changes.contains { $0.kind == .restored })
    }

    @Test func relocateChecksTheGame() throws {
        let (s, _) = store()
        let out = tmp()
        let app = fakeApp(in: out, titleId: "0100000000010000")
        let e = try s.add(app: app, info: BuiltAppInfo.read(app)!, bytes: nil)
        try FileManager.default.removeItem(at: app)
        _ = s.refresh()
        let wrong = fakeApp(in: tmp(), name: "Other", titleId: "01006BB00C6F0000")
        #expect(throws: LibraryError.differentGame("Game")) { try s.relocate(e.id, to: wrong) }
        let right = fakeApp(in: tmp(), name: "Game2", titleId: "0100000000010000")
        try s.relocate(e.id, to: right)
        #expect(s.status(of: e.id) == .present)
        #expect(s.entries[0].path == right.standardizedFileURL.path)
        #expect(throws: LibraryError.notNappleApp) { try s.relocate(e.id, to: tmp()) }
    }

    @Test func adoptPicksUpUnknownMacAppsOnly() throws {
        let (s, _) = store()
        let out = tmp()
        fakeApp(in: out, name: "One", titleId: "0100000000010000")
        fakeApp(in: out, name: "Two", titleId: "01006BB00C6F0000")
        fakeApp(in: out, name: "Phone", titleId: "0100000000020000", mac: false)
        try Data([1]).write(to: out.appendingPathComponent("junk.txt"))
        #expect(s.adopt(directory: out) == 2)
        #expect(s.adopt(directory: out) == 0)                   // second scan finds nothing new
        #expect(Set(s.entries.map(\.name)) == ["One", "Two"])
    }

    @Test func removeAndCorruptFile() throws {
        let (s, root) = store()
        let app = fakeApp(in: tmp())
        let e = try s.add(app: app, info: BuiltAppInfo.read(app)!, bytes: nil)
        s.remove(e.id)
        #expect(s.entries.isEmpty)
        try Data("not json".utf8).write(to: root.appendingPathComponent("library.json"))
        let reopened = LibraryStore(file: root.appendingPathComponent("library.json"))
        #expect(reopened.entries.isEmpty)                       // starts empty instead of failing
        #expect(FileManager.default.fileExists(atPath: root.appendingPathComponent("library.json.bad").path))
    }
}

@Suite struct CompatDisplayTests {
    @Test func countsAndAttentionNote() {
        let m = CompatMatrix.parse(Data(#"""
        {"games":[{"title_id":"AA","name":"Game","versions":[{"kind":"base","stages":{
          "ingest":{"status":"pass"},"ingame":{"status":"partial","reason":"3D picture is black (gfx)"},"saves":{"status":"partial","reason":"x"}}}]}]}
        """#.utf8))!
        let e = m.entry(for: "aa")!
        #expect(e.passCount == 1)
        #expect(e.attentionNote == "In game: 3D picture is black (gfx)")
        #expect(CompatEntry.stageTitle("title-screen") == "Title screen")
        #expect(CompatEntry.stageTitle("odd-stage") == "Odd Stage")
    }

    @Test func passingGameHasNoAttentionNote() {
        let m = CompatMatrix.parse(Data(#"{"games":[{"title_id":"BB","versions":[{"kind":"base","stages":{"ingame":{"status":"pass"}}}]}]}"#.utf8))!
        #expect(m.entry(for: "BB")?.attentionNote == nil)
    }

    @Test func planSummaries() throws {
        let p = try PlanResult.decode(Data(PlanTests.json.utf8))
        #expect(p.selectedUpdateVersions == [65536])
        #expect(p.selectedDLCIds.isEmpty)
        #expect(p.includedSummary == "1 update")
    }
}
