# Studio redesign: one window, a library of built games

## Objective
Rebuild Napple Studio's interface (SwiftUI, same package, same `napple` CLI contract) around a design board: a fixed-size glass window with a
library home screen, an add screen, a plan screen, live build progress, a done screen, an in-window Settings page and a first-launch setup page.
The engine integration (`docs/studio-protocol.md`, `docs/ios.md`, first-launch `EngineInstaller`) is unchanged.

## What changed
- **Library home** (`LibraryView.swift`): every built macOS app as a card (icon read from the app, version, size, compatibility from `compat/matrix.json`,
  Launch, Show in Finder, Install to /Applications, Remove, Move to Trash). Dragging files anywhere in the window shows a drop overlay and opens the plan.
- **Library follows the files** (`NappleStudioCore/Library.swift`, `<support>/library.json`): macOS builds only. Each entry keeps its path and a bookmark;
  `refresh()` runs at launch, on app activation and (debounced) when a watched folder changes. A moved or renamed app is followed through its bookmark; an app moved
  to the Trash leaves the library; an app that vanished stays, flagged, with Locate… (must be the same title id) and Remove. Apps already sitting in the output
  folder (built by the CLI or an older Studio) are adopted by their `NappleTitleId`. A corrupt library file is set aside as `library.json.bad`.
- **One window, fixed 900 x 620**: screens switch inside it (`Screen` in `AppModel.swift`); Settings is a page (⌘,), not a second window; back arrow beside
  the traffic lights; quitting during a build asks first and cancels cleanly.
- **Plan screen**: base/update/DLC lists, compatibility card (overall status, stage count, what still fails, all stages), destination menu
  (This Mac, connected iOS 26+ devices, iOS/iPadOS .ipa), key-set menu, Signing sheet for iOS targets, disk and output-folder warnings, Add files… for updates/DLC.
- **Edge cases**: non-game or missing drops are reported instead of silently ignored; `.xcz` is accepted (the CLI already scans it); duplicates are merged; drops
  during a build are refused with a message; drops before any keys exist wait and are planned right after the keys are saved; no base game; plan failure with retry;
  unwritable output folder; disk space; build finished without an app; cancel returns to the plan; failed build shows the error and log with Try again;
  the Mac stays awake during a build; Dock bounce when a build ends in the background; sandboxless bookmark resolution without UI or mounting.
- Tests: `Tests/NappleStudioTests/LibraryTests.swift` (input expansion, app info, library add/dedupe/move/missing/restore/relocate/adopt/corrupt file, compat display helpers).

## Files
`studio/Sources/NappleStudio/{StudioApp,AppModel,RootView,LibraryView,KeysViews,PlanView,BuildDoneViews,Components}.swift` (old `Views.swift` removed);
`studio/Sources/NappleStudioCore/{Library,Inputs}.swift` (new), `Compat.swift` and `Plan.swift` (display helpers).

## Not verified
There is no Swift toolchain on the Linux box this was written on: nothing here has been compiled or run, and the tests have not been executed. Run
`swift build` and `swift test` in `studio/` on a Mac with Xcode 26 first and fix any compile errors. Also still to check by eye: traffic-light alignment with the
custom header (hidden title bar), the `.containerBackground` glass over a real desktop, and menu rows for devices.

## Decisions
- Only macOS apps are listed in the library; iOS builds still end on the done screen (install on device, .ipa).
- The built app's icon is read from the app itself; before a build the plan screen shows a controller tile because the game's own icon is not available yet.
- "Follow the file" uses Finder-style bookmarks plus a folder watcher rather than polling.
