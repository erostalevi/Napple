# Napple video: shot list, recording checklist, pre-flight

Shot numbers match [script.md](script.md). `A01` is the presenter's A-roll (face to camera, used all over the script). Anything marked **scripted** can be triggered with Napple's own
tooling so the take is repeatable; the variables are documented in [../../reference/environment-variables.md](../../reference/environment-variables.md).
Legal rule for every capture: game footage only from games the presenter owns, no keys, firmware paths, serial numbers or personal data visible (crop or blur terminals and Finder),
no real amiibo artwork unless the licence is clear (a placeholder card is fine).

Status words: *planned* = not captured yet. Fill the "Done" column during the shoot.

## Shots

Columns: device, game, scene, length, capture settings, how to trigger.

| # | Script chapter | Device | Game / subject | Scene | Len | Capture settings | How to trigger | Done |
|---|---|---|---|---|---|---|---|---|
| A01 | all | Camera (face) | Presenter | Talking head, desk with the Mac and a TV in soft focus behind | n/a | 4K 24 or 30 fps (match the screen-recording rate in the edit), 1/50 shutter, lav mic | read [script.md](script.md); record each block twice | [ ] |
| S01 | Cold open | Mac | Terminal | Black terminal, one command typed: `napple build` on a placeholder file name (no real path) | 3 s | screen recording 1080p60 | type by hand; blank the prompt path first | [ ] |
| S02 | Cold open | Mac | LAS | Gameplay full screen, statistics HUD in a corner showing fps | 6 s | native Retina, 60 fps | overlay statistics on (F1, Audio/Video tab); `NAPPLE_PERF_LOG=1` as a second source for the numbers; play by hand | [ ] |
| S03 | Cold open | TV | LAS | Same game on the TV, controller in hands (only hands visible) | 4 s | camera 4K30, locked off, TV brightness lowered to avoid blooming | Mac on HDMI, fullscreen (Cmd+Return) | [ ] |
| S04 | Legal | Camera | Presenter | Legal statement, plain background, no music | 40 s | as A01 | read the lines in script.md | [ ] |
| S05 | Ch1 | Mac | Animation | Four highlighted instructions: x18, thread pointer, svc, timer counters (+ 4K to 16K pages) | 50 s | 1080p60 motion graphics | built from [../../architecture/rewrite-format.md](../../architecture/rewrite-format.md); no game code shown, use a synthetic listing | [ ] |
| S06 | Ch1 | Mac | Animation | Five-stage pipeline: ingest, analyze, resolve, rewrite, package | 30 s | 1080p60 | redraw the README mermaid flowchart | [ ] |
| S07 | Ch2 | Mac | Terminal | Runtime log scrolling with `opened service ...` lines | 15 s | screen recording 1080p60 | headless run: `runtime/build/napple-runtime <data dir> --headless --seconds 20 --save-root <tmp dir>`; scroll the log in a pager | [ ] |
| S08 | Ch2 | Mac | COMPATIBILITY.md | Zoom on "Services requested by every game above (42)" | 8 s | 1080p | open the file in a browser, zoom | [ ] |
| S09 | Ch2 | Mac | LAS | A gameplay frame with the Metal HUD on | 8 s | native, 60 fps | overlay Audio/Video tab, native Metal HUD toggle | [ ] |
| S10 | Ch2 | Mac | Docs page | Slow zoom on [../../architecture/gfx.md](../../architecture/gfx.md) (docs only, no game shaders) | 8 s | 1080p | browser | [ ] |
| S11 | Ch3 | Mac | Napple Studio | Library grid (empty state, then with one built app) | 8 s | 1440p or native, 60 fps, dark and light mode variants | launch Studio; `--pane` flag selects a pane, `--debug-tour <dir>` writes pane rectangles for screencapture | [ ] |
| S12 | Ch3 | Mac | Napple Studio | Dragging a game file onto the window (use a Finder window with only the file name visible) | 8 s | as S11 | drag by hand; first-run setup sheet shown once with a throwaway key set copy folder | [ ] |
| S13 | Ch3 | Mac | Napple Studio | Plan view: base, updates, DLC toggles | 8 s | as S11 | drop a game; wait for the plan | [ ] |
| S14 | Ch3 | Mac | Napple Studio | Build progress with the stage checklist, then the log inspector (option-command-L) | 15 s | as S11, speed-ramp the wait | press Build (command-B); log filter box visible | [ ] |
| S15 | Ch3 | Mac | Terminal | `napple plan`, `napple build ... --data-only --update-romfs in-place`, folder size of the output | 20 s | screen recording 1080p60, large font | commands from [../../titles/README.md](../../titles/README.md); blur the key path | [ ] |
| S16 | Ch4a | Mac | LAS | Windowed play, a few minutes of footage to cut from | 3 min raw | native Retina, 60 fps, system audio via loopback | double-click the built app; for repeatable takes use a scripted input: `NAPPLE_INPUT_SCRIPT="15:A,19:A"` (seconds after HID is configured : button[:hold]) | [ ] |
| S17 | Ch4a | Mac | LAS | Window to fullscreen toggle | 5 s | native | Cmd+Return | [ ] |
| S18 | Ch4a | Mac | LAS | Close-up of the statistics HUD (fps, frame time) | 6 s | zoomed crop of S16 | overlay statistics; or `NAPPLE_PERF_LOG=1` lines in a terminal for a graphic | [ ] |
| S19 | Ch4a | Mac | SMO | 20 s B-roll of intro, menu and Cascade Kingdom; no broken frames used as a "win" | 20 s | native, 60 fps | `NAPPLE_INPUT_SCRIPT="30:A:0.3,45:A:0.3,60:A:0.3"` boots the first kingdom (see [../../titles/0100000000010000.md](../../titles/0100000000010000.md)); stop before ~2 minutes of idle play (the known glitch) | [ ] |
| S20 | Ch4b | Camera | Room | Wide shot of the room: Mac on a stand, HDMI cable, TV | 6 s | 4K30 | tripod | [ ] |
| S21 | Ch4b | Camera | Presenter | Over-the-shoulder, controller in hands, TV in frame | 8 s | 4K30 | pair the controller beforehand (system Bluetooth) | [ ] |
| S22 | Ch4b | TV | LAS | Close-up of the TV with gameplay | 15 s | 4K30 locked off; exposure set on the screen | fullscreen on the HDMI display; console mode docked in the overlay Controls tab | [ ] |
| S23 | Ch4c (B) | Mac / iOS Simulator | LAS | Title screen on the Simulator | 10 s | screen recording of the Simulator window | run per [../../architecture/ios.md](../../architecture/ios.md): the title screen appears by about 60 s; reuse an earlier recording | [ ] |
| S24 | Ch4c (B) | Mac / iOS Simulator | SMO | Start menu on the Simulator | 8 s | as S23 | as S23 (the title note says touch delivery from tooling did not work: use a recording of the menu, do not promise play) | [ ] |
| S25 | Ch4c (B) | Mac | Napple Studio | iPhone & iPad pane: devices list, signing card, exact command lines | 12 s | as S11 | open the pane; show the command lines, hide Apple ID and team id | [ ] |
| S26 | Ch4c (B) | Mac | iOS overlay | Screenshots of the touch overlay (only if a Simulator build shows it) | 6 s | stills | Simulator: `SIMCTL_CHILD_NAPPLE_FE_OVERLAY=<tab> xcrun simctl launch ...` (see audio-input-frontend.md) | [ ] |
| S27 | Ch4c | Camera | Devices | iPhone and iPad on the desk | 5 s | 4K30 | tripod | [ ] |
| S28 | Ch4c (A only) | iPhone | LAS | Game on the device | 20 s | iPhone screen recording + camera over the shoulder | only if the pre-flight gate "device boots a title" passes; say the iOS version and device model on screen | [ ] |
| S29 | Ch4c (A only) | iPad | LAS | Game on the iPad with the on-screen controller | 15 s | as S28 | as S28 | [ ] |
| S30 | Ch4c (A only) | iPhone | Overlay | Touch overlay, swipe to close | 10 s | as S28 | as S28 | [ ] |
| S31 | Ch5 | Mac | LAS | Overlay master take: all four tabs in order | 90 s | native, 60 fps; mouse cursor visible and slowed | F1 or Cmd+,; for deterministic tab openers use `NAPPLE_FE_OVERLAY=<0..3>` (0 Audio/Video, 1 FPS/Speed, 2 Controls, 3 Game/System) | [ ] |
| S32 | Ch5 | Mac | LAS or any game that reads amiibo | Amiibo list, search, Place, Remove (placeholder entry; no real amiibo artwork) | 20 s | as S31 | Game/System tab; headless proof for the description: `NAPPLE_AMIIBO=<model id>,2000,8000` | [ ] |
| S33 | Ch5 | Mac | Overlay | Camera section: permission state, mirror toggle | 10 s | as S31 | Game/System tab; scripted IR source for headless proof: `NAPPLE_IR_SOURCE=blob:160,120,30` | [ ] |
| S34 | Ch5 | Mac | Mii Maker | New Mii flow: Create New Mii, Start from Scratch, part editor with the live 3D head, name prompt, Done | 60 s | native, 60 fps | `napple build --firmware-program 0100000000001009 --firmware <zip> -k <keys> -o <dir>`, run the data dir; answer the name prompt with `NAPPLE_SWKBD_ANSWER=Eros`; proof line `mii: list ... wrote 2` in the log of a game (MK8D) | [ ] |
| S35 | Ch6 | Mac | Graphic | Three columns: works today / rough edges / what is next (table in script.md) | 12 s | 1080p | built from script.md chapter 6 | [ ] |
| S36 | Ch7 | Mac | Graphic | CTA list animation (nine items, from [cta.md](cta.md)) | 20 s | 1080p | built from cta.md | [ ] |
| S37 | Outro | Mac | Graphic | Outro card: repo link, Discord/forum placeholder, end-screen slots | 20 s | 1080p | cta.md "Outro card" | [ ] |

## Recording checklist

Screen (Mac)
1. macOS 26 or later, Do Not Disturb on, notifications off, menu-bar clutter hidden, desktop clean, a neutral wallpaper; dark mode for Studio takes, plus one light-mode variant.
2. Record with QuickTime or the system screenshot toolbar at native resolution; 60 fps if the recorder allows it, otherwise record the game with the app's own screenshot function for stills and use a camera for motion. Keep the game window at 1x for the "honest" takes and mention the scale in the lower third.
3. Capture system audio with a loopback device (BlackHole or similar, your own licence); check the game audio level against dialogue before each take (game peaks below -20 dBFS).
4. Mouse: enable pointer highlight; move slowly; click, then pause 0.5 s.
5. Terminals: font 20+ pt, dark theme, prompt shortened to `$`, history cleared, working directory hidden, no environment variable containing paths to keys.

TV / HDMI
6. Connect by HDMI, set the display to 1080p60 or 4K60 (check in System Settings), fullscreen in the game; console mode docked in the overlay.
7. To show the TV truthfully shoot it with a camera (S22); if an HDMI capture card is available, capture the HDMI output of a second source only if the Mac's own display mirroring allows it (capture cards can add HDCP or latency issues: do a 30-second test first).
8. Lock exposure and white balance on the camera; reduce TV backlight; turn off motion smoothing; avoid moire (slightly defocus or angle the camera).

iPhone / iPad
9. Simulator takes: window at 100 percent, no other Simulator windows. Physical device takes (only version A): cable and QuickTime movie recording, or Control Center screen recording; Do Not Disturb on; hide notifications; Developer Mode on; free-account apps expire after seven days, rebuild the day before.
10. Film the device in hand with a second camera for the physical "feel" shot; keep reflections off the glass.

Audio
11. Lavalier or shotgun mic, gain so speech peaks around -12 dBFS; room tone 30 s at the end of each setup; claps at the start of takes for sync.
12. Target mix: dialogue -16 LUFS integrated, music -26 to -30 LUFS under speech, true peak below -1 dBTP.

Frame rates and resolutions
13. Timeline 3840x2160 at 30 fps (camera) with 60 fps screen captures conformed with frame blending off; export 4K and 1080p H.264 or HEVC, with burned-in captions as a separate caption file as well.
14. Every gameplay clip carries the corner chyron `Recorded on <Mac model, chip> · Napple <commit>` for credibility.

## Pre-flight checklist

Builds
- [ ] Latest `main` built from a clean checkout: `cargo build --release -p napple-cli`, `make -C runtime`, `make -C packaging`, `studio/build_app.sh` (see [../../../README.md](../../../README.md) quick start).
- [ ] Record the commit hash and macOS version on a card; put them in the chyron and the description.
- [ ] Run the title build the day before; data in the small in-place mode is fine for the Mac takes.
- [ ] Headless smoke test per title: 45 s, exit 0, no `CRASH` ([../../titles/README.md](../../titles/README.md)).

Titles and saves
- [ ] LAS (hero): fonts present in `<save-root>/system/fonts` so text renders ([../../symptoms.md](../../symptoms.md)); a fresh save-root and a save with the first scene reached.
- [ ] SMO: a save at the start of the first kingdom; do not film past about two minutes of idle (known issue).
- [ ] MK8D: update 3.0.5 plus DLC data built; a pre-scripted route into a race (`NAPPLE_INPUT_SCRIPT` from the title note); check the corner fps (40-47).
- [ ] ACNH: only needed if a B-roll of the opening is wanted; mark it "work in progress" on screen.
- [ ] Firmware for the Mii Maker take built (`--firmware-program 0100000000001009`); Mii database starts empty (use a temp `NAPPLE_MII_DIR`), so the take is clean and the user's real Miis stay untouched.
- [ ] Amiibo take uses a placeholder amiibo (own virtual entry), and the community database source is set before recording so the list is populated.
- [ ] Camera take: camera permission prompt tested once beforehand.

Devices
- [ ] External display and HDMI cable tested; controller paired and charged; fullscreen toggles; console mode docked.
- [ ] iOS gate: has any game booted on a physical iPhone or iPad with the current build (run it once, unscripted)? Yes: script Version A for chapter 4c. No: Version B (Simulator + honest "not yet"). Decide the evening before; do not decide in the edit.
- [ ] Simulator recordings of LAS and SMO made in advance (the docs say the title screen needs about 60 s).

Safety / legal
- [ ] No prod.keys, title.keys, firmware names or full file paths on screen; crop or blur.
- [ ] No game files shared or linked; no thumbnails with ripped assets beyond a screenshot of the user's own running game; the video does not offer builds.
- [ ] Music licences collected; no Nintendo music or sound effects used as bed.

Backups
- [ ] Copy raw footage to a second drive after each setup; keep the Napple commit hash with the footage folder.
