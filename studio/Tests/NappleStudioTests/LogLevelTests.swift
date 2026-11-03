import XCTest
@testable import NappleStudioCore

final class LogLevelTests: XCTestCase {
    func testErrors() {
        XCTAssertEqual(LogLevel.classify("error: nifm:u unsupported command 21"), .error)
        XCTAssertEqual(LogLevel.classify("[hle] ERROR: bad handle"), .error)
        XCTAssertEqual(LogLevel.classify("build FAILED"), .error)
        XCTAssertEqual(LogLevel.classify("thread 'main' panicked: panic at x"), .error)
        XCTAssertEqual(LogLevel.classify("fatal signal 11"), .error)
    }

    func testWarnings() {
        XCTAssertEqual(LogLevel.classify("warning: 3 shaders use an unsupported opcode"), .warning)
        XCTAssertEqual(LogLevel.classify("[gfx] warning: replay path"), .warning)
        XCTAssertEqual(LogLevel.classify("WARN slow frame"), .warning)
    }

    func testPlainLinesAreInfo() {
        XCTAssertEqual(LogLevel.classify("[gfx] frame 600, 59.8 fps"), .info)
        XCTAssertEqual(LogLevel.classify("$ napple build --check game.nsp"), .info)
        XCTAssertEqual(LogLevel.classify("plan: base 4.4 GB, update 1.0.2"), .info)
        XCTAssertEqual(LogLevel.classify(""), .info)
    }

    func testOrdering() {
        XCTAssertTrue(LogLevel.info < LogLevel.warning)
        XCTAssertTrue(LogLevel.warning < LogLevel.error)
    }
}
