# Studio final: real `napple build` integration

## Objective
Studio drives the full pipeline (drop -> plan -> choose updates/DLC -> build -> done) with the real `napple build`.

## Files
- `studio/Sources/NappleStudioCore/NappleCLI.swift`: real flags (`--updates/--dlc/--firmware/--name/--runtime/--launcher/-o`), `build --check` preflight,
  `error: [code] message` -> human text (`ErrorText`), cancel = SIGTERM (Task cancel -> `Process.terminate`).
- `Compat.swift`: reads the real matrix (`games[].versions[].stages`), per-stage list, overall status derived from stages (no top-level status exists):
  ingame pass=Playable, partial=Partial; else title-screen pass=Menus; else runtime-boot=Boots; fail/blocked=Nothing.
- `AppModel.swift`, `Views.swift`: output folder (default `~/Applications/Napple`), disk-space preflight, build log files, done screen
  (Launch, Reveal, Install to /Applications, notes such as codesign warnings), per-stage compat banner, Settings (sets, output folder, logs).
- `studio/build_app.sh`: builds CLI, `runtime/` and `packaging/` if missing (`NAPPLE_REBUILD=1` forces) and bundles `napple`, `napple-runtime`,
  `napple-launcher`, `matrix.json` into `Resources/`. Studio passes `--runtime/--launcher` from there.
- Debug flags: `--debug-drop`, `--debug-keys`, `--debug-firmware`, `--debug-ack`, `--debug-autobuild` (builds when plan ready, exits 0/1),
  `--debug-exit-delay <s>`, `--support-dir`, `--output-dir`.
- Tests: 12 (added humanize, derived compat status).

## Decisions
- Keyset firmware zip is passed to every build (fonts). `--title-keys` is passed but ignored by the core today.
- Matrix lookup: repo `compat/matrix.json` when run from a checkout, else bundled copy.
- Logs: `~/Library/Application Support/Napple/logs/build-*.log` (CLI output only, 0600).
- Disk need estimate = selected pack sizes + 1 GB (conservative; the core does its own exact check).

## Risks
- macOS asks for Documents access when the app reads files there via paths it was not handed by drop/picker; drag-drop and the file picker grant it.
  The automated run used APFS clones in /private/tmp to avoid the prompt.
- Overall compat status is a heuristic until the matrix gets a game-level status.
- Compat per-stage list is long; it is collapsed by default but the plan area scrolls.

## Verified (real run)
Studio.app launched with --debug-keys/--debug-firmware/--debug-drop/--debug-autobuild on Link's Awakening: plan 6.27 GB, build produced
`~/Applications/Napple/The Legend of Zelda Link's Awakening.app` through the UI, `codesign --verify --deep --strict` OK,
`napple-launcher --headless --seconds 15`: 721 frames. Screenshots in `work/shots/studio-*.png`.
