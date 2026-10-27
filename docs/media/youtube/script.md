# Napple video: shooting script

Working title: **"I didn't emulate the Switch. I changed four things."** (title options: [titles-thumbnails-description.md](titles-thumbnails-description.md))
Target length: **12:30** (range 10-14 min). Language: English, captions on. Presenter: Eros J. Talevi (face to camera).
Companion files: [cta.md](cta.md) (call to action), [shotlist.md](shotlist.md) (every shot, S01-S34), [README.md](README.md).

How to read this file: `ON CAM` = presenter speaking to the lens. `VO` = voice-over on footage. `TEXT` = on-screen text. `SHOT Sxx` = a row of the [shotlist](shotlist.md).
`[STATUS: ...]` marks a claim with its honest status words (verified / tested / untested / coming). Anything not marked is verified by a scripted run recorded in the repo.
Nothing in this video is shown that the repo does not document; if a shot cannot be captured on shoot day, use the fallback in the shotlist and drop the claim.

The idea that makes this video different from "yet another emulator demo": the whole video is built on one counter-intuitive fact (the Switch and your Mac are both ARM64, so
nothing is emulated) and one honest rule ("every claim has a log line"). Every chapter ends with a number or a screen the viewer can check themselves.

---

## Cold open (0:00-0:15)  [hook]

| Time | Picture | Sound |
|---|---|---|
| 0:00-0:03 | SHOT S01: black screen, a single line of terminal text appears: `$ napple build "Zelda.nsp"` and the cursor blinks. | One low synth note, no music yet. |
| 0:03-0:09 | SHOT S02: smash cut to Link's Awakening running full screen on the Mac, 60 fps counter in the corner (overlay statistics, `NAPPLE_PERF_LOG` style HUD). Presenter's voice over it. | Game audio under the voice, ducked. |
| 0:09-0:12 | SHOT S03: TV in a living room showing the same game, controller in the presenter's hands (hands only). | Beat drop (see music notes). |
| 0:12-0:15 | ON CAM, close, direct: the line below. TEXT: `NOT AN EMULATOR`. | Cut music to silence for the last word. |

VO / ON CAM (0:02-0:15):
> "This is a Nintendo Switch game. It's running as a native Mac app. No emulator, no JIT, no tricks with a second CPU. I'm going to show you what I actually changed to make it run,
> and the answer is: four things."

TEXT at 0:13: `4 things. Not 4 million.`  (the four things are named in chapter 1: the reserved register, the thread register, system calls and timer counters.)

---

## Legal, up front (0:15-0:55)

SHOT S04: presenter on camera, plain background, no music. This goes early on purpose and is repeated in the description and pinned comment.

ON CAM:
> "Before anything else, the legal part, because it matters and because you should hear it from me and not from a comment section.
> Napple contains no game, no keys and no firmware. Not one byte. You bring your own legally dumped game, your own prod.keys and your own firmware, from a console you own.
> Napple uses them once, on your machine, to build an app for you, and that app is for your personal use. Please don't share builds, and please never post keys, firmware or games in the issues.
> Napple isn't affiliated with or endorsed by Nintendo. It's a research and preservation-minded project by one person. OK. Now the fun part."

TEXT (lower third, 0:20-0:50): `Bring your own game, keys and firmware. Nothing proprietary in the repo. Personal use only. Not affiliated with Nintendo.`
Sources: README "Legal, up front", CONTRIBUTING.md.

---

## Chapter 1: "Wait, it's not an emulator?" (0:55-3:00)

Goal: the viewer understands the architecture in two minutes and believes it.

0:55-1:25 ON CAM, standing next to a whiteboard or a simple animated diagram (SHOT S05 for the animation, built from the README pipeline diagram).
> "A Switch has an ARM64 processor. Your Apple Silicon Mac has an ARM64 processor. Same instruction set. So why would I translate the instructions? I don't.
> The game's own machine code runs directly on your CPU. What I do is find the handful of things macOS will not allow, and rewrite those, ahead of time, once, when you build the app."

1:25-2:15 VO over an animated sequence (SHOT S05): a block of code with four highlighted instructions, one at a time, each with a label.
> "One: register x18. On macOS it's reserved by the platform, so the game can't use it the way the Switch does. I rewrite it.
> Two: the thread pointer register. The Switch keeps thread-local storage there, macOS has its own idea. Rewritten.
> Three: system calls. The game asks the Switch's operating system, Horizon, for everything. Those calls are intercepted and sent to my implementation of Horizon.
> Four: the timer counter registers. Games read the clock directly. Those reads are rewritten too.
> And one more detail: the Switch uses 4-kilobyte memory pages, Apple Silicon uses 16-kilobyte pages. The rewriter deals with that too. That's the whole CPU story."

TEXT sequence (one per beat): `x18` / `thread pointer` / `svc (system calls)` / `timer counters` / (small) `4K -> 16K pages`.
Sources: README "What is this?", architecture/overview.md, architecture/rewrite-format.md.

2:15-2:45 VO over the pipeline diagram (SHOT S06, the mermaid flowchart from the README redrawn as an animation): ingest, analyze, resolve, rewrite, package.
> "Five steps, all on your machine: read the game container, find the code, look up known function signatures, rewrite, and package it into a normal .app that is signed locally. And it's done ahead of time. At run time there's no recompiler. No JIT. Ever. That's also why this approach can work on iPhone and iPad, where JIT is not allowed."

TEXT: `ingest -> analyze -> resolve -> rewrite -> package`; `No JIT, ever.`

2:45-3:00 ON CAM, the humor beat #1 (self-deprecating):
> "Notice what I did not say: 'easy'. The CPU is the easy half. The other half is pretending to be a whole console operating system and a GPU. We'll get to that."

Sources: README "What is this?" and "FAQ: Is this an emulator?"; docs/architecture/ios.md "Why iOS needed more than a new target string" (no JIT, AOT only).

---

## Chapter 2: "The other half": Horizon and the GPU (3:00-4:30)

3:00-3:40 VO over screen-capture of a terminal log (SHOT S07): scrolling runtime log with `opened service ...` lines; then a zoom on the list of services requested by a game (SHOT S08, from COMPATIBILITY.md "Services requested by every game above (42)").
> "When a Switch game wants to save a file, play sound, read the controller or ask what time it is, it talks to services. Horizon has dozens of them. Super Mario Odyssey asks for forty-two. Napple answers each one itself: it's a high-level re-implementation, not a copy of Nintendo's code, and the services that need the internet answer offline."

TEXT: `42 services requested by Super Mario Odyssey` (COMPATIBILITY.md, services used (42)).

3:40-4:10 VO over SHOT S09 (a game frame with the Metal HUD from the overlay, showing frame time) and SHOT S10 (a slow wipe between a shader source snippet and the final picture; use a screenshot of the docs, not game shaders).
> "The graphics are the other mountain. The Switch's GPU API, NVN, gets translated to Metal, and the shaders written for the console's Maxwell GPU get translated too. The project estimates that its coverage of the platform is around eighty-seven percent. And I want to be clear what that number is: it's coverage of the platform surface, not how many games work. Coverage is not compatibility."

TEXT: `Platform coverage estimate: ~87%` then immediately `coverage != compatibility` (README Status; both lines come from there).

4:10-4:30 ON CAM, quick:
> "Per-game knowledge lives in data, signatures and patch tables, never in per-game code. That's a design rule I keep fighting myself to follow."

---

## Chapter 3: "Drop a game on it" (Napple Studio) (4:30-6:00)

4:30-5:30 SHOT S11-S14 (screen recording of Napple Studio, no cuts longer than 6 s). Presenter VO.
> "You can do all of this from a terminal, and I'll show that in a second. But there's an app for it: Napple Studio, built in SwiftUI with the new Liquid Glass look on macOS 26.
> First launch, you point it at your keys and firmware once. Then you drop a game on the window. It plans the build, shows you the base game, the updates and the DLC it found, and you press Build. You get real stage names, a checklist, and a log. When it's done, the app lands in your Library, and double-click plays it."

Cues: S11 library grid (empty state then with covers, only if covers can be shown without game art leaking to the description; see shotlist legal note), S12 drop zone, S13 plan view with toggles, S14 build progress with the stage checklist and the log inspector (option+command+L).
TEXT: `Napple Studio: Library, Add Game, iPhone & iPad, Log`; `⌘O add game · ⌘B build · ⌥⌘L log`.
Sources: docs/guides/studio.md, archive/changes/2026-10-26-claude-studio-redo-2026-10-26.md.

5:30-6:00 VO over SHOT S15 (terminal, for the nerds): `napple plan`, then `napple build ... --data-only --update-romfs in-place`, showing the size difference.
> "And for the terminal people: ingest, plan, build. There's even a mode that keeps the game data in place instead of copying it: about two hundred and fifty megabytes instead of up to nineteen gigabytes. That's a development mode, and I use it all day."

TEXT: `~250 MB instead of 5-19 GB (development mode)`. Source: docs/titles/README.md "Building test data without disk pain".
Humor beat #2 (ON CAM, 5:55): "My SSD sent me a thank-you card."

---

## Chapter 4: "Three screens" (the showcase) (6:00-8:30)

The structure: the same game, three places. Keep the three sequences visually distinct: Mac = windowed, bright desk setup; TV = living room, wide shot plus over-the-shoulder; iPhone/iPad = hands, handheld shot.
Hero title for the whole chapter: **The Legend of Zelda: Link's Awakening** (the most mature title: plays, 60 fps live windowed in a 3-6 minute play, 58 fps headless; docs/titles/01006BB00C6F0000.md).

### 4a. The Mac (6:00-6:50)
SHOT S16: LAS windowed on the Mac, desktop recording, then S17: fullscreen toggle (Cmd+Return), then S18: close-up of the performance statistics from the overlay (frame time, fps).
VO:
> "First stop, the Mac. Link's Awakening, windowed, native, sixty frames a second in live play, GPU time six to twelve milliseconds a frame on an M-series chip. This is the title that works best today."

Optional second beat (6:35-6:50): SHOT S19 SMO intro/menu and Cascade Kingdom for twenty seconds, B-roll only, with the voice line:
> "Super Mario Odyssey runs too: intro, menus, the first kingdom, sixty fps live. It has visual bugs I'll tell you about later, because I'd rather you hear them from me."
[STATUS: SMO plays; visual bugs open per docs/titles/0100000000010000.md; use only footage that does not show a broken frame as a "win".]
Sources: docs/titles/README.md, 01006BB00C6F0000.md, 0100000000010000.md.

### 4b. The TV (6:50-7:40)
SHOT S20: wide shot of the living room, Mac closed or on a stand connected by HDMI. S21: over-the-shoulder, presenter holding a game controller. S22: the TV close-up with the game, filmed with a locked-off camera (camera footage, not Mac screen capture, so the TV is actually seen).
VO:
> "Second stop: the TV. The Mac is plugged in by HDMI, the game is fullscreen, and a regular controller is paired. There's a console mode setting, docked or handheld, so the game thinks it's docked at 1080p when it's on the TV. It's the Switch experience, except the 'Switch' is a MacBook."

Cues: fullscreen is Cmd+Return or Ctrl+Cmd+F; controller via GameController framework (rumble supported in code, input verified through keyboard; controller hardware coverage is partial, so show a controller in hand only if it was verified on the shoot day; otherwise film hands on a keyboard-mapped setup and say nothing about rumble).
[STATUS: fullscreen and console mode exist; the TV run is shot on the day: if it does not look right, cut the section and say "TV next time".]
Humor beat #3 (ON CAM, 7:30): "Yes, the fan noise is also authentic. The Switch has a fan too."  (keep only if the fan is audible on the take).
Sources: docs/architecture/audio-input-frontend.md sections 2.2b, 164-173 (fullscreen, console mode).

### 4c. The iPhone and iPad (7:40-8:30)
This section has two versions. The pre-flight checklist in [shotlist.md](shotlist.md) decides which one is used on shoot day. Default is **Version B**, because the repo says plainly that nothing has run on a physical device yet (docs/architecture/ios.md; README Status).

**Version B (default, honest): the Simulator and the road to a device.**
SHOT S23: the iOS Simulator on the Mac showing the Link's Awakening title screen (recorded earlier; the docs record it reaching the title screen by about 60 s), S24: Super Mario Odyssey start menu on the Simulator, S25: Studio's iPhone & iPad pane (devices list, signing card, the command lines it runs), S26: the on-screen controller overlay design (screenshots of the iOS overlay, only if a build exists), S27: an iPhone and an iPad on the desk, screens off or on the Studio pane.
VO:
> "Third stop: iPhone and iPad. And this is where I owe you the honest version. The iOS build compiles, and it runs in the iOS Simulator: Link's Awakening gets to its title screen, Mario Odyssey to its start menu. On a real phone? Not yet. I have not run a game on a physical iPhone or iPad. I'm telling you that now so that nobody has to find out in the comments.
> The reason it's even plausible is the no-JIT design. iOS doesn't let apps generate code at run time, and Napple never does. Executable code is wrapped into signed libraries at build time, and Studio can sign, install and launch on a connected device with your own Apple ID. That path is written. It isn't proven on hardware. If you have an iPhone and want to help prove it, that's literally one of the ways you can contribute."
TEXT: `iOS: runs in the Simulator. Physical device: NOT YET.` and at the end `Help wanted: iPhone & iPad testers`.
[STATUS: iOS build compiled and run on the Simulator (verified); device install path written, untested on hardware; iOS work is paused in the current roadmap until macOS is done; say "next", not "soon".]

**Version A (only if the pre-flight gate "iOS device boots a title" passes before the shoot): the real device.**
SHOT S28: iPhone screen recording (QuickTime over cable or Control Center recording) of the game on the device, S29: iPad held in hands with the on-screen controller, S30: the iOS overlay (touch panel, swipe right to close).
VO: replace the paragraph above with: "And here it is on a real iPhone, built by Studio, signed with my own Apple ID, no jailbreak, no JIT. [then describe exactly what is on screen, and say the measured fps from the overlay]". Do not claim anything the capture does not show. Add TEXT with the device model, iOS version and fps.
Sources: docs/architecture/ios.md; docs/guides/studio.md "iPhone and iPad".

---

## Chapter 5: "The toolbox in the overlay" (8:30-10:00)

Press F1 (or Cmd+, or the controller Home button) in a running game: a glass panel opens on the right and the game keeps running behind it.
SHOT S31 is the master take of the overlay; sub-takes S32-S34 are inserts. The voice walks through the four tabs with real cursor movement.

8:30-9:00 Audio/Video tab (NAPPLE_FE_OVERLAY=0):
> "Press F1 and you get the overlay. Video: render scale from half-resolution up to 4x, supersampling, an upscaler, output filter, sharpness, an on-screen statistics HUD, even Apple's native Metal HUD. FPS and speed: vsync, frame generation, game speed. Controls: remap everything, per player."
TEXT: `Render scale 0.5x / 1x / 2x / 4x · supersampling · upscaler · frame generation`.
[STATUS: the controls exist; a deeper pass on custom scale values, ultrawide and portrait output, and a built-in upscaler is in progress and is shown as "coming" with a lower-third, not demonstrated.]
Sources: audio-input-frontend.md overlay panel paragraph.

9:00-9:30 Game/System tab: amiibo and IR camera (NAPPLE_FE_OVERLAY=3):
> "Hardware the console has and your Mac doesn't gets a simulator instead of being dropped. Amiibo: a virtual amiibo, searchable, placed and removed from the overlay, with no dumps or keys required. Names and pictures come from a community database that is fetched on your machine; the repo ships none of it. And the IR camera: if a game wants it, your webcam stands in, and the app asks for camera permission only when the game actually starts using it."
Cues: S32 amiibo list with search and Place; S33 Camera tab with the permission state. Do not show real amiibo art unless it is a placeholder; legal note in the shotlist.
[STATUS: simulators are implemented and unit tested in portable C; the amiibo/IR behaviour was exercised with scripted sources; compatibility with real amiibo dumps is unverified (docs/guides/hardware-simulators.md). Say "simulated", never "100% compatible".]

9:30-10:00 The Mii Maker, from your own firmware:
> "And my favourite one. The Mii Maker, the actual system applet, runs from your own firmware. Create a new Mii, edit the face, hair, eyes, the 3D head renders live, save it, and then a game, here Mario Kart, can read that Mii from a shared database. It's still the real thing: Napple doesn't ship any of it, it builds it from the firmware you supply."
Cues: SHOT S34: Mii Maker run (`napple build --firmware-program 0100000000001009`, then run the data directory), one scripted flow: Create New Mii, part editor, name prompt, Done; then the Mii list log line `mii: list ... wrote 2`.
[STATUS: verified on macOS with headless scripted input and screenshots (archive/changes/2026-10-26-mii-firmware.md). Mii faces on in-game racers in MK8D were NOT confirmed there: do not claim them.]

---

## Chapter 6: "What works, what is rough, what is next" (10:00-11:30)

Rule: this chapter is the credibility engine. Full screen graphic of three columns, presenter ON CAM between the beats, no music or a very low bed. Numbers come from docs/titles/README.md and COMPATIBILITY.md.

TEXT card (hold for 10 s, S35 is the graphic): 

| Works today | Rough edges | What is next |
|---|---|---|
| Link's Awakening plays at 60 fps live | Stutter when a new area first loads shaders (170-1300 ms) | Render scale, ultrawide and portrait output, an own upscaler |
| Super Mario Odyssey: intro, menus, first kingdom | Visual bugs in SMO after minutes of play (exploded geometry, stale command lists) | A shareable mod and fix format (`.napmod`) |
| Mario Kart 8 Deluxe: races at 40-47 fps (update + DLC) | MK8D tops out near 45 fps; graphics warnings in the log | Zero graphics bugs on the four test titles, golden frames |
| Animal Crossing: boots through the opening and shows a picture | ACNH picture depends on a stopgap; about 30 fps | Real fix instead of the stopgap |
| Studio, overlay, simulators, Mii Maker | iPhone and iPad: Simulator only | Physical iPhone/iPad, then catalogue and support tables |

ON CAM (10:00-11:30), delivered across the table:
> "Here's the honest state of things. What works: Link's Awakening is solid. Mario Odyssey plays through the first kingdom. Mario Kart 8 races at forty to forty-seven frames a second. Animal Crossing gets through the opening.
> What's rough: Zelda stutters the first time you walk into a new area, while shaders build. Mario Odyssey develops visual glitches after a few minutes. Mario Kart is stuck near forty-five fps. Animal Crossing shows its picture through a stopgap I'm not proud of, and runs at about thirty.
> Only four games have been tested. 'Which games work?' has an honest answer: those four, and the rest is unknown. That is exactly where you come in.
> What's next, in order: finish macOS, make it correct and fast; then iPhone and iPad; then the catalogue of which games work, with a proper support table. Windows and Linux are not goals."

TEXT: `Tested: 4 games. Everything else: unknown.`
Humor beat #4 (ON CAM at the ACNH line): "A stopgap is a very Nintendo word for 'I'll fix it later.' It is not a very good word for a graphics pipeline."  (keep or cut: do not mock anybody except the presenter; this one is self-directed.)
Sources: docs/titles/*.md, COMPATIBILITY.md, README Status and FAQ ("Which games work?"), docs/reference/support-matrix.md.

---

## Chapter 7: Call to action (11:30-12:45)

Full detail and link text in [cta.md](cta.md). The script keeps it as a spoken ask plus an on-screen list. The ask is specific: viewers should leave knowing one thing they can do in 30 minutes.

ON CAM (11:30-12:30):
> "This is a one-person project and it needs more people. You don't need to know what an NSO is. Here's what you can do.
> If you can write, document a game: the title notes in docs slash titles and the support table need entries, and it's a great first contribution. If you can mod, write fixes and mods; a shareable mod format is coming. If you find a bug, report it with the log. If you know graphics, help with golden frames. If you speak another language, translate. If you have a different Mac or an older chip, test on it and tell me the result. If you have an iPhone or iPad and you're brave, help me prove the device path. If you just like it, star the repo or sponsor it, it genuinely helps.
> And please, only with your own games, keys and firmware. Never attach those to an issue.
> The link is in the description. Start with the 'good first issue' list [PROPOSED in cta.md until the labels exist]."

TEXT list (animated, one per beat, S36): `Document a title` / `Mods & fixes` / `Bug reports with logs` / `Graphics goldens` / `Translations` / `Test on more Macs & chips` / `iPhone & iPad testing` / `Studio UI feedback` / `Star or sponsor`.
Outro card (12:30-13:00, S37): `github.com/erostalevi/Napple` plus the Discord/forum placeholder, end-screen elements (subscribe, a second video slot).

Outro VO (12:35-12:45):
> "That's Napple. Four things changed, one console operating system re-implemented, and a GPU translated. Thanks for watching, and see you on GitHub."

---

## Pacing notes
- Face-to-camera blocks are never longer than 40 s; cut to footage every 5-8 s inside chapters 3-5. Chapter 1 gets motion graphics, not gameplay (the viewer wants to understand, not to watch).
- The three-device chapter alternates wide / close / screen, never two same-type shots in a row.
- Chapter 6 is slow on purpose; a quiet beat after "the rest is unknown" (2 s of silence) before the CTA.
- Put a small recurring chyron in a corner of any gameplay footage: `Recorded on <Mac model, chip> · Napple <commit>`. It builds the "every claim has a log line" trust.
- Subtitles burned in for TEXT-heavy sections; keep legal lower third at least 30 s on screen in total.

## Music and sfx (royalty-free guidance only)
- Use only music you have a licence for: YouTube Audio Library, or CC0 / CC-BY tracks with credit in the description, or a paid royalty-free library whose licence allows monetised YouTube use. Keep the licence and download receipt in the project folder.
- Do not use game music from the video's subject games as background (the in-game audio heard in captured footage is incidental; keep it low and brief). Do not use any soundtrack that sounds like a Nintendo theme.
- Moods: cold open = sparse electronic pulse with a hit on the cut; legal = silence or a very low pad; chapters 1-2 = curious, light percussion, 100-110 bpm; chapter 3 = clean, upbeat; chapter 4 = warm, driving; chapter 5 = playful; chapter 6 = near-silent bed; CTA = rising, hopeful, then resolve on the outro card.
- SFX: keyboard clicks for the terminal, a soft "whoosh" for each of the four things, a UI "pop" for the CTA list. Normalise dialogue at -16 LUFS integrated, music at -26 to -30 LUFS under speech, true peak below -1 dBTP.

## Humor beats (all self-directed, nobody mocked)
1. 2:45 "Notice what I did not say: easy."
2. 5:55 "My SSD sent me a thank-you card."
3. 7:30 "The fan noise is also authentic." (only if audible)
4. 10:50 "A stopgap is a very Nintendo word..." (cut if it reads as mocking the company; swap for "A stopgap is a bug wearing a name tag.")

## Checks before locking the edit
- Every shot number above exists in [shotlist.md](shotlist.md) (S01-S37) and every shotlist row names the script chapter that uses it.
- Legal text is on screen for at least 30 s and spoken in the first minute.
- No keys, firmware paths, game files, console serials or personal data visible in any terminal or Finder window (blur or crop).
- The thumbnails and title do not claim support for games that are not in the four tested.
- The "coming" claims (render scale upgrades, mods, physical iOS device) stay marked as coming and never get a demo.

## Sources (every claim traces to the repo)
| Claim | Source |
|---|---|
| Not an emulator; ARM64 native; rewrite of x18, thread register, svc, counters; 4K to 16K pages; no JIT | README.md (What is this?, FAQ); docs/architecture/overview.md; docs/architecture/rewrite-format.md |
| Five pipeline stages, signed locally, per-game data not code | README.md (diagram, Per-game knowledge) |
| Legal framing (own game, keys, firmware, personal use, not affiliated) | README.md "Legal, up front"; CONTRIBUTING.md |
| 42 services requested by Super Mario Odyssey; services list | COMPATIBILITY.md |
| Platform coverage ~87% estimate; coverage is not compatibility | README.md "Status" |
| Studio features, Liquid Glass, Library/Add Game/Log, shortcuts | docs/guides/studio.md; docs/archive/changes/2026-10-26-claude-studio-redo-2026-10-26.md |
| In-place data mode ~250 MB vs 5-19 GB | docs/titles/README.md |
| LAS 60 fps live, 58 fps headless, stutter on new areas | docs/titles/01006BB00C6F0000.md |
| SMO plays, 60 fps live, open visual bugs | docs/titles/0100000000010000.md |
| MK8D races 40-47 fps (update + DLC) | docs/titles/0100152000022000.md |
| ACNH opening, stopgap, ~30 fps | docs/titles/01006F8002326000.md |
| Support table, only four titles tested | docs/reference/support-matrix.md; README.md FAQ |
| Overlay (F1, tabs, render target 0.5x-4x, supersampling, upscaler, frame generation, Metal HUD, console mode) | docs/architecture/audio-input-frontend.md |
| Fullscreen (Cmd+Return, Ctrl+Cmd+F), controller Home opens overlay | docs/architecture/audio-input-frontend.md |
| Amiibo and IR camera simulators, camera permission on demand, no data shipped | docs/guides/hardware-simulators.md; studio-redo change note |
| Mii Maker from firmware, shared Mii database, verified flow | docs/archive/changes/2026-10-26-mii-firmware.md |
| iOS: Simulator only, no JIT, signed dylibs, device untested | docs/architecture/ios.md; README.md Status |
| Scripted capture env vars (NAPPLE_INPUT_SCRIPT, NAPPLE_SCREENSHOT, NAPPLE_FE_OVERLAY, NAPPLE_PERF_LOG, NAPPLE_AMIIBO, NAPPLE_IR_SOURCE) | docs/reference/environment-variables.md |
| Coming: render scale upgrades, mods format, contributor kit | planned batch files orchestrations/render-scale.md, mod-framework.md, contributor-kit.md (NOT shipped; say "coming") |
| Roadmap order (macOS first, iOS later, catalogue; Windows/Linux not goals) | README.md; orchestrations/macos1-common.md |
