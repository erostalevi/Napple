import Foundation

/// Device discovery, install and launch through `xcrun devicectl` (hardware) and `xcrun simctl` (Simulator).
public struct DeviceService: Sendable {
    public let tools: ToolRunner
    public init(tools: ToolRunner = .system) { self.tools = tools }

    /// Paired phones and tablets (`devicectl list devices --json-output`).
    public func physicalDevices() async throws -> [IOSDevice] {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("napple-devices-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: file) }
        _ = try await tools.xcrun(["devicectl", "list", "devices", "--json-output", file.path])
        guard let data = try? Data(contentsOf: file) else { return [] }
        return DeviceList.parse(data)
    }

    /// Booted iOS Simulators (`simctl list devices booted -j`).
    public func bootedSimulators() async throws -> [IOSDevice] {
        let out = try await tools.xcrun(["simctl", "list", "devices", "booted", "-j"])
        return SimulatorList.parse(Data(out.stdout.utf8))
    }

    /// Hardware first, then booted simulators. A failure of one source does not hide the other.
    public func allDevices() async -> (devices: [IOSDevice], note: String?) {
        var all: [IOSDevice] = [], note: String?
        do { all += try await physicalDevices() } catch { note = "Could not list devices: \(error.localizedDescription)" }
        if let sims = try? await bootedSimulators() { all += sims }
        return (all, note)
    }

    public func install(app: URL, on d: IOSDevice) async throws {
        let args = d.simulator ? ["simctl", "install", d.id, app.path] : ["devicectl", "device", "install", "app", "--device", d.id, app.path]
        do { _ = try await tools.xcrun(args) }
        catch { throw CLIError.failed(code: 1, message: ErrorText.humanize("error: [install] \(error.localizedDescription)")) }
    }

    public func launch(bundleId: String, on d: IOSDevice, environment: [String: String] = [:]) async throws {
        var args: [String]
        if d.simulator { args = ["simctl", "launch", "--terminate-running-process", d.id, bundleId] }
        else { args = ["devicectl", "device", "process", "launch", "--device", d.id, "--terminate-existing", bundleId] }
        _ = environment   // devicectl takes --environment-variables as JSON; none are passed by default
        do { _ = try await tools.xcrun(args) }
        catch { throw CLIError.failed(code: 1, message: "Installed, but launching failed: \(error.localizedDescription) Trust the developer in Settings > General > VPN & Device Management, then open the app.") }
    }

    /// The exact command lines (for docs and the UI's "copy command").
    public static func commandLines(app: URL, bundleId: String, on d: IOSDevice) -> [String] {
        d.simulator
            ? ["xcrun simctl install \(d.id) \(app.path)", "xcrun simctl launch \(d.id) \(bundleId)"]
            : ["xcrun devicectl device install app --device \(d.id) \(app.path)", "xcrun devicectl device process launch --device \(d.id) \(bundleId)"]
    }

    /// An .ipa is a zip with Payload/<Name>.app; devicectl and simctl want the bundle.
    public func unpack(ipa: URL, into dir: URL) async throws -> URL {
        try? FileManager.default.removeItem(at: dir)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        _ = try await tools.run(URL(fileURLWithPath: "/usr/bin/ditto"), ["-x", "-k", ipa.path, dir.path])
        let payload = dir.appendingPathComponent("Payload")
        guard let app = (try? FileManager.default.contentsOfDirectory(at: payload, includingPropertiesForKeys: nil))?.first(where: { $0.pathExtension == "app" }) else {
            throw CLIError.failed(code: 1, message: "The .ipa has no Payload/*.app.")
        }
        return app
    }
}

public enum SimulatorList {
    public static func parse(_ data: Data) -> [IOSDevice] {
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let groups = root["devices"] as? [String: Any] else { return [] }
        var out: [IOSDevice] = []
        for (runtime, v) in groups {
            guard runtime.contains("iOS") || runtime.contains("iPadOS"), let list = v as? [[String: Any]] else { continue }
            let version = runtime.split(separator: ".").last.map { $0.replacingOccurrences(of: "iOS-", with: "").replacingOccurrences(of: "-", with: ".") } ?? "?"
            for d in list where (d["state"] as? String) == "Booted" {
                guard let id = d["udid"] as? String else { continue }
                var dev = IOSDevice(id: id, name: d["name"] as? String ?? "Simulator", model: "Simulator", osVersion: version, connected: true, developerMode: true)
                dev.simulator = true
                out.append(dev)
            }
        }
        return out.sorted { $0.name < $1.name }
    }
}
