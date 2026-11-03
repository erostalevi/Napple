# Napple Studio

Napple Studio is the Mac app that turns a Switch game file into a native Mac app, keeps the apps it built in a library, and builds, signs, installs and exports the
same game for iPhone and iPad. It is a SwiftUI app for macOS 26 (translucent window, system typography and symbols, full dark/light, VoiceOver, keyboard
navigation, menu bar commands). Sources: `studio/` (`NappleStudioCore` = testable logic, `NappleStudio` = the app).

## Layout

One fixed-size window (1000 x 680, no resizing or full screen) that shows only the library. Everything else opens on top of it.

| Part | What it does |
|---|---|
| **Library** | Grid of covers (the app icon, else the name on a colour), edge to edge. Click selects, Play shows on hover and selection, double-click or Return plays. Right-click: Play, Play with Debug, Show in Finder, Move to Trash, Remove from Library. A game whose app is gone is simply not shown (it returns by itself if the app comes back). Search in the header; empty and no-result states. The header holds Add Game and the gear (Settings). |
| **Add / Build panel** | Slides in from the right for the whole flow. Reading files -> what to include (base, updates, DLC), warnings and blockers (no base game, low disk, engine missing, compatibility notes) -> *This Mac* (save folder, play when done) or *iOS Device* (install to a device or export an .ipa for iPhone or iPad, signing, bundle id, launch after install, debug build) -> build progress with the live log -> done (Play / Show in Finder / notes, install progress and retry for a device) or failed (reason, last log lines, Try Again). Closing the panel during a build keeps it running: the game's cover shows a progress ring and a click reopens the panel. |
| **Play with Debug** | Starts the game as its own process and opens a console window with its live log: filter, All/Warnings/Errors, follow, clear, copy, save, stop. |
| **Dialogs** | First launch: keys and firmware are required before anything else. "Personal use only" before the first build. "A build is still running" on quit. Dropping files anywhere in the window starts the Add flow (so do the Dock icon and File > Open). |
| **Settings** (gear, ⌘,) | Eight tabs: General (output folder, play after build, legal notice), Keys (key sets: create and delete; the set created last is in use), iPhone & iPad (devices, signing mode, team or identity and profile, bundle id prefix, memory limit, install an existing .ipa or .app), Mii, Amiibo, Online, Engine (installed versions with Remove, and releases on GitHub with Install) and Log (the last build log). |

The Mii, Amiibo and Online tabs write `~/Library/Application Support/Napple/system.json` (one contract: the runtime reads it at boot, see [system data folders](../reference/system-data-folders.md)); they use `NappleCLI.swift`, the same code the build uses, and have no second code path. `studio/Sources/NappleStudioBuild` is the same build path without a window (`napple-studio-build`), which `tools/e2e/run.sh` uses.

First launch asks for the folder that holds `prod.keys`, `title.keys` and the firmware (Studio finds them), or each file. Files are copied to
`~/Library/Application Support/Napple/keysets/<name>/` (owner-only); key values are never read or logged (only key names are checked). If the keys are deleted
later the library stays usable, building is paused and a banner points to Settings > Keys.

Menu bar: File > Add Game (⌘O), Build (⌘B), Cancel Build (⌘.); Device > Refresh (⌘R), Install .ipa or .app; Help > logs, this guide.

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
`--debug-ack` (legal notice), `--debug-autobuild` (build and exit 0/1), `--no-engine-setup`, `--debug-screen <name>` (puts the window in a fixed state with fake data so a
screen can be photographed without a game or a device: `add`, `add-ios`, `blockers`, `reading`, `building`, `building-closed`, `done`, `failed`, `legal`, `drop`,
`selected`, `search`, `console`), `--debug-light` (light appearance), `--debug-settings <tab>` (opens Settings on a tab: `general`, `keys`, `devices`, `mii`, `amiibo`,
`online`, `engine`, `log`). Photograph a window with `screencapture -l <window id>`.
