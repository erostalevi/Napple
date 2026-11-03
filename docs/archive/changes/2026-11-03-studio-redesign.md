# 2026-11-03 studio-redesign: library-only window, Add / Build panel, Play with Debug, new Settings window

Branch `studio-redesign`, Mac session. Design: a canvas (boards 1-29) worked out with the product owner; the decisions below are the final ones from its comments.

## What changed

### Main window (`studio/Sources/NappleStudio`)
- `RootView`: one fixed 1000 x 680 window (frame fixed in `WindowAccessor.configure`, zoom and full screen off), vibrancy background, header with Add Game, search and the
  gear. `LibraryView`: covers edge to edge, Play on hover/selection, double-click and Return play, context menu (Play, Play with Debug, Show in Finder, Move to Trash,
  Remove from Library), empty and no-result states, progress-ring tile for a running build, keys-removed banner. Missing games are not shown.
- `Panel/`: `PanelHost` (switches on `Phase`), `PlanPanel` (include, Mac or iOS options, notices), `BuildPanels` (building with live log, done with iOS install
  progress and retry, failed with the log tail), `ReadingPanel`. Closing the panel during a build keeps the build running (`AppModel.closePanel`).
- `Dialogs`: required first-launch keys dialog, legal dialog, drop overlay, toast. `ConsoleWindow` + `DebugConsole`: Play with Debug.
- Removed: sidebar, step bar, wizard, log sheet, `AddGameView`, `DevicesView`, the debug tour. Model: `Pane`/`showInspector` gone, `panelOpen`, `closePanel`,
  `AppModel+Destination` (Mac or iOS device, install targets), stale-plan guard (`planGeneration`), cached cover images, `playAfterBuild` (UserDefaults).
- `DebugScreens`: `--debug-screen <name>` fixed states with fake data for screenshots.

### Settings (`Views/Settings/`, `AppModel+Settings`)
- Eight tabs. Keys: create and delete only; the set created last is active (`ActiveKeyset`). Engine: installed versions with Remove (`EngineStore.remove`
  refuses `current`, the bundled engine, links and anything outside the engine folder) and releases of `erostalevi/napple` with Install (`EngineStore`: release
  JSON, SHA256SUMS check, `ditto` extraction, atomic move, pruning to 5, never repoints `engine/current`).

### Core
- `LogLevel` (error/warning/info from the words of a line), `EngineStore`, `KeysetActive`.

## Verified
- `swift build`, `swift test` (94 tests, new: `LogLevelTests`, `EngineStoreTests`).
- Screenshots of every main-window state and all eight Settings tabs (`--debug-screen`, `--debug-settings`).
- A real Mac build of a test game through the new code path (`--debug-drop` + `--debug-autobuild`, exit 0, 6.1 GB app written).
- Play with Debug against a scripted fake game: lines stream live, levels colour and filter.

## Not verified
- Installing on a physical iPhone or iPad, Developer Mode and real-profile signing states.
- Clicking through the UI by hand (no accessibility access here): menus, drag and drop, the pickers and sheets were exercised only in fixed states and by unit tests.
- Light mode (colours follow the system, but no light screenshot was captured), a real download and install from GitHub (unit-tested with a fake zip), `swiftlint`/`swiftformat` (not installed).
