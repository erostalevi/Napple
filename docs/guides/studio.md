# Napple Studio

Napple Studio is the Mac app that turns a Switch game file into a native Mac app, keeps the apps it built in a library, and builds, signs, installs and exports the
same game for iPhone and iPad. It is a SwiftUI app for macOS 26 (Liquid Glass, system typography and symbols, full dark/light, VoiceOver, keyboard navigation,
window restoration, menu bar commands). Sources: `studio/` (`NappleStudioCore` = testable logic, `NappleStudio` = the app).

## Layout

| Pane | What it does |
|---|---|
| **Library** | Grid of the Mac apps Studio built (cover art from the app icon, version, size, compatibility dot). Click selects (action bar: Play, Show in Finder), double-click plays, right-click for Copy to Applications, Move to Trash, Locate (for a moved app), Remove. Apps that are moved or renamed in Finder are followed by bookmark; trashed apps leave the library. Search in the toolbar. |
| **Add Game** | Drop zone (the whole window accepts drops, so does the Dock icon and File > Open) -> plan (base, updates, DLC toggles, destination, blockers) -> build progress with the real stage names and a stage checklist -> done (Play / Install / Show in Finder / notes) or the failure with the reason. |
| **iPhone & iPad** | Paired devices (`devicectl`) and booted Simulators (`simctl`), the guided signing flow, the memory-limit decision, "Install .ipa or .app…", and the exact command lines Studio runs. |
| **Log** (inspector, ⌥⌘L) | The live build log with filter, copy, and "show logs in Finder" (`~/Library/Application Support/Napple/logs/build-*.log`). |
| **Settings** (⌘,) | General (output folder, install/launch after build, legal notice), Keys & Firmware (sets, add/replace/delete), Engine (which `napple` is used). |

First launch shows the setup sheet: choose the folder that holds `prod.keys`, `title.keys` and the firmware (Studio finds them), or pick each file. Files are copied to
`~/Library/Application Support/Napple/keysets/<name>/` (owner-only); key values are never read or logged (only key names are checked).

Menu bar: File > Add Game (⌘O), Build (⌘B), Cancel Build (⌘.); Go > the panes (⌘1-3); Device > Refresh (⌘R), Install on Device; View > Show/Hide Log; Help > logs, this guide.

## Building for a Mac

Drop the game (and, optionally, its updates and DLC files or the folder that holds them). Studio runs `napple plan --json`, shows the base, the updates and the
DLC with sizes, then `napple build --check` and `napple build` with the bundled runtime and launcher. Everything ticked is embedded and can be switched in the
game's own overlay (F1). The app is written to the output folder (default `~/Applications/Napple`; keep it outside Desktop/Documents so the signature stays valid)
and added to the library.

## iPhone and iPad

Studio talks to devices through Xcode's tools; no third-party helper:

```
xcrun devicectl list devices --json-output <file>          # discovery (hardware)
xcrun simctl list devices booted -j                        # discovery (Simulator)
xcrun devicectl device install app --device <id> <App>.app
xcrun devicectl device process launch --device <id> <bundle id>
xcrun simctl install <udid> <App>.app ; xcrun simctl launch <udid> <bundle id>
```

A device shows as *Ready*, *Not connected*, *Developer Mode off* or *Needs iOS 26*. Pick it as the destination of a build (or export an `.ipa` for iPhone or iPad).
With "Install after building" on, the app is installed and launched as soon as the build ends; "Install .ipa or .app…" installs an existing file.

### Bundle ids

Every game gets its own bundle id, `<prefix>.<titleid>` (default prefix `com.napple.game`), so several games can live on a device at the same time. The prefix
is editable in Signing. A non-wildcard provisioning profile pins its own id instead; Studio says so and suggests Automatic signing, which gets one id per game.

### Signing modes

| Mode | What happens | Needs |
|---|---|---|
| **Automatic (Xcode)** | Before the build Studio writes a throwaway stub Xcode project with the game's bundle id, your team and the entitlements, and runs `xcodebuild … -allowProvisioningUpdates -allowProvisioningDeviceRegistration build`: Xcode registers the App ID, turns on its capabilities, registers the connected device and downloads a development profile. `napple build` then signs with that profile and the matching certificate and writes the `.ipa`. (`xcodebuild -exportArchive` cannot be used: it refuses archives it did not build, "certificateType DEVELOPMENT_MANAGED has an invalid value".) | An Apple ID signed in to Xcode (Settings > Accounts) with an Apple Development certificate. |
| **Provisioning profile** | `napple build --signing-identity … --provisioning-profile …`: the CLI signs with the profile's own entitlements. | A certificate and a profile already on this Mac (`napple build --list-signing`). |
| **Unsigned** | Unsigned app/`.ipa` for AltStore, Sideloadly or your own tools. Cannot be installed from Studio. | Nothing. |

Simulators are a fourth case: `--target ios-sim`, ad-hoc signed, installed with `simctl`, no signing at all.

Automatic mode always makes a development profile (your registered devices); for Ad Hoc or App Store distribution export from Xcode with your own project.

### Free or paid account, and the memory limit

iOS caps an app's memory; big games need the **Increased Memory Limit** capability (`com.apple.developer.kernel.increased-memory-limit`, plus
`com.apple.developer.kernel.increased-debuggable-memory-limit` for debug builds). Only paid Apple Developer Program accounts can use it, so Studio decides per build
(`AccountDetector`, `IOSEntitlementPlan` in `NappleStudioCore/SigningAccount.swift`):

1. Xcode's own team list (`IDEProvisioningTeamByIdentifier`: `Personal Team` / `isFreeProvisioningTeam`) wins;
2. else a profile that already grants the capability means paid;
3. else the profile lifetime: 7 days = free Apple ID, 60 days or more = paid;
4. else a team name ending in "(Personal Team)" = free; anything else = unknown, treated like free.

Paid: the entitlements go into the stub project's `.entitlements`, so `-allowProvisioningUpdates` turns the capability on for the App ID and the profile Xcode
returns grants it; `napple build` signs with the profile's entitlements. With a fixed profile that lacks it Studio says so. Free or unknown: nothing is added, the build is normal and the UI explains that large games may be closed by iOS. Nothing else is ever added: no JIT, no private
API. Both branches are unit-tested (`MemoryEntitlementTests`).

## Amiibo and the IR camera

These are per game and live in the game's own overlay (F1 in a running game: *amiibo* and *Camera* tabs), not in Studio. See
[hardware-simulators.md](hardware-simulators.md). The game app's `Info.plist` carries `NSCameraUsageDescription`; the webcam prompt appears only when a game starts
the IR camera.

## Building and testing Studio

```
cd studio
swift build            # or: ./build_app.sh [out-dir]   -> NappleStudio.app with napple, napple-runtime, napple-launcher (+ napple-runtime-ios[-sim] when built)
swift test             # NappleStudioCore unit tests (CLI protocol, library, keysets, signing/account, entitlements, export, devices, bundle ids)
```

Debug flags of the app binary: `--support-dir <dir>`, `--output-dir <dir>`, `--debug-keys <prod.keys> [--debug-firmware <zip>]`, `--debug-drop <file>`,
`--debug-ack` (legal notice), `--debug-autobuild` (build and exit 0/1), `--no-engine-setup`, `--pane library|add|devices`, `--debug-tour <dir>`
(visits every pane and hands the window rectangle to an external `screencapture`; with `--debug-tour-build` it also builds the dropped game: the scripted UI check).
