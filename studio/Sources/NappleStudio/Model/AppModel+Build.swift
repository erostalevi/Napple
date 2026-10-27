import SwiftUI
import AppKit
import NappleStudioCore

extension AppModel {
    var diskWarning: String? { plan.flatMap { diskPreflight($0) } }

    var outputWarning: String? {
        let p = outputDir.path, home = FileManager.default.homeDirectoryForCurrentUser.path
        if p.hasPrefix(home + "/Desktop") || p.hasPrefix(home + "/Documents") || p.contains("Mobile Documents") {
            return String(localized: "The output folder is in a synced location. Pick a folder outside Desktop and Documents so the app's signature stays valid.")
        }
        return nil
    }

    var canBuild: Bool { plan?.base != nil && phase != .planning && phase != .building && selectedSet != nil && cli != nil }

    func startBuild() {
        guard phase != .building else { return }
        guard let plan, let set = selectedSet, let cli else { return }
        guard plan.base != nil else { errorText = String(localized: "Add the base game before building."); return }
        guard legalAck else { showLegal = true; return }
        if let msg = diskPreflight(plan) { fail(msg); return }
        if let why = destinationBlocker { fail(why); return }
        let out = outputDir
        do { try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true) }
        catch { fail(String(localized: "Cannot create the output folder \(out.path): \(error.localizedDescription)")); return }
        guard FileManager.default.isWritableFile(atPath: out.path) else { fail(String(localized: "Studio cannot write to \(out.path). Choose another output folder in Settings.")); return }

        let ios0 = iosOptions()
        var req = BuildRequest(files: files, keyset: set, updates: plan.selectedUpdateVersions, dlc: plan.selectedDLCIds,
                               target: target.rawValue, output: out, ios: ios0)
        builtSummary = plan.includedSummary
        phase = .building; pane = .add
        errorText = nil; notes = []; installMessage = nil; cancelling = false
        builtApp = nil; builtIPA = nil; builtInfo = nil; builtBytes = nil; buildDuration = nil; stagesSeen = []
        buildStarted = Date()
        progress = ProgressEvent(stage: "preflight", percent: 0, message: String(localized: "Checking keys and firmware")); log = []
        openLog()
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Building a Napple app")
        let isMac = target == .macos
        let entPlan = entitlementPlan
        buildTask = Task {
            defer { if let a = activity { ProcessInfo.processInfo.endActivity(a); activity = nil } }
            do {
                try await cli.check(keyset: set)
                req.ios = try await provisionIfNeeded(ios0)
                let app = try await cli.build(req) { ev in Task { @MainActor in self.handle(ev) } }
                try? await Task.sleep(nanoseconds: 200_000_000)
                guard let result = app ?? firstApp(in: out), FileManager.default.fileExists(atPath: result.path) else {
                    throw CLIError.failed(code: 1, message: String(localized: "The build finished but no app was written to \(out.path). Check the log for warnings."))
                }
                builtApp = result
                builtInfo = BuiltAppInfo.read(result)
                if !isMac { try await finishIOS(app: result, plan: entPlan, output: out) }
                buildDuration = buildStarted.map { Date().timeIntervalSince($0) }
                writeLog("DONE \(result.path)")
                if isMac { await registerInLibrary(result) } else { builtBytes = await Task.detached { BuiltAppInfo.sizeOnDisk(result) }.value }
                // The install/launch step may have handed focus to the Simulator or the device tools: switch screens on a fresh run-loop turn,
                // never from inside whatever AppKit pass resumed this task.
                await Task.yield()
                phase = .done
                if !NSApp.isActive { DispatchQueue.main.async { NSApp.requestUserAttention(.informationalRequest) } }
                finishDebug(0)
            } catch {
                if Task.isCancelled || cancelling || error.localizedDescription == "Build cancelled." {
                    phase = .ready; errorText = nil
                    say(String(localized: "Build cancelled."))
                } else {
                    phase = .failed; errorText = error.localizedDescription
                    if !NSApp.isActive { NSApp.requestUserAttention(.informationalRequest) }
                }
                cancelling = false
                writeLog("FAILED \(error.localizedDescription)")
                finishDebug(1)
            }
        }
    }

    private func fail(_ msg: String) { errorText = msg; phase = .failed; finishDebug(1) }

    func diskPreflight(_ plan: PlanResult) -> String? {
        let need = plan.totalSelectedBytes + 1_000_000_000
        var probe = outputDir
        while !FileManager.default.fileExists(atPath: probe.path), probe.path != "/" { probe.deleteLastPathComponent() }
        guard let free = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage,
              UInt64(free) < need else { return nil }
        return String(localized: "Not enough disk space: about \(PlanResult.formatBytes(need)) needed, \(PlanResult.formatBytes(UInt64(free))) free. Free some space or choose another output folder in Settings.")
    }

    func cancelBuild() {
        guard phase == .building else { return }
        cancelling = true
        buildTask?.cancel()
    }

    func backToPlan() { errorText = nil; phase = plan == nil ? .idle : .ready }

    func handle(_ ev: BuildEvent) {
        switch ev {
        case .progress(let p):
            progress = p
            if stagesSeen.last != p.stage { stagesSeen.append(p.stage) }
            let l = "[\(p.stage)] \(Int(p.percent))% \(p.message)"
            appendLog(l); writeLog(l)
        case .result(let p): appendLog("result: \(p)"); writeLog("RESULT \(p)")
        case .log(let l):
            appendLog(l); writeLog(l)
            if l.hasPrefix("note:") || l.hasPrefix("warning:") { notes.append(l) }
        }
    }

    func firstApp(in dir: URL) -> URL? {
        let apps = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []).filter { $0.pathExtension == "app" }
        return apps.max { a, b in
            let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return da < db
        }
    }

    /// Human names for the CLI's stage ids.
    static func stageName(_ s: String) -> String {
        switch s {
        case "preflight": String(localized: "Checking keys")
        case "provision": String(localized: "Provisioning with Xcode")
        case "ingest": String(localized: "Reading game files")
        case "plan", "build-plan": String(localized: "Planning")
        case "rewrite": String(localized: "Recompiling the code")
        case "romfs": String(localized: "Copying game data")
        case "fonts": String(localized: "System fonts")
        case "bundle": String(localized: "Packaging the app")
        case "sign": String(localized: "Signing")
        case "done": String(localized: "Finished")
        case "ipa": String(localized: "Writing the .ipa")
        case "install": String(localized: "Installing")
        default: s.replacingOccurrences(of: "-", with: " ").capitalized
        }
    }
}
