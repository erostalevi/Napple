# Napple video: titles, thumbnails, description, pinned comment, Shorts, thread

Companion of [script.md](script.md), [cta.md](cta.md) and [shotlist.md](shotlist.md). Every claim here is one the script backs with a source; do not strengthen them.
Rule for all packaging: say "native" and "no emulation / no JIT" rather than promising game compatibility; only four titles have been tested.

## 10 title options

1. I Didn't Emulate the Switch. I Changed 4 Things. (recommended, matches the cold open)
2. Switch Games as Native Mac Apps (No Emulator, No JIT)
3. I'm Building a Switch-to-Mac Native Recompiler Alone
4. Zelda on a Mac, on a TV, and (Almost) on an iPhone: How It Works
5. The Switch and Your Mac Speak the Same CPU Language. So I Used That.
6. Running Switch Games Natively on Apple Silicon: The Honest Progress Report
7. One Developer, One Mac, One Native Switch Recompiler (Help Wanted)
8. Why Switch Games Don't Need an Emulator on Apple Silicon
9. Four Registers That Stand Between a Switch Game and Your Mac
10. I Made a Mac App from a Game I Own. Here's What Works and What Doesn't.

Title hygiene: keep under 70 characters for the visible part; the "legal" word choice is "a game I own"; do not use "free games", "download" or "ROM".

## 5 thumbnail concepts

All thumbnails use gameplay of the creator's own running game or the Napple UI, never ripped art or logos. Text at most four words, large, high contrast. Check at 160 px wide.

1. **"NOT AN EMULATOR"**: left half the presenter's face (surprised, lit from the side), right half a Link's Awakening frame in a Mac window. Text `NOT AN EMULATOR` in white on a dark band, a small `4 things` tag in the accent color. Colors: graphite background, one warm orange accent, white text.
2. **Three screens**: a triptych (Mac window, TV, phone silhouette) all showing the same game frame, thin lines connecting them; the phone has a red `SOON` stamp (honest: Simulator only). Text `3 SCREENS`. Colors: deep teal and white, orange stamp.
3. **Four registers**: close-up of code with four highlighted rows, a big `x18` in the accent color, the presenter small in the corner pointing. Text `4 THINGS`. Colors: black, neon green highlight, white.
4. **Apple + controller**: a Mac with a Studio window "Drop a game here" glowing, a controller floating beside it. Text `DROP A GAME`. Colors: Liquid Glass-style frosted panel over a dark gradient, white text.
5. **Help wanted**: a plain poster style: repo name, a star, a checklist with three ticks (`Docs`, `Mods`, `Bugs`) and the presenter smiling. Text `HELP WANTED`. Colors: cream background, black text, one blue accent. (Use for a later community-focused upload or A/B test.)

Do not put a Nintendo logo, character art or the word "Switch" in a form that implies an official product; "Switch" in plain text is fine.

## YouTube description (copy and paste; replace placeholders)

```
Napple turns a Switch game you own into a native Apple Silicon app. No emulator, no JIT: your Mac and the Switch are both ARM64, so the game's own code runs directly and I rewrite, ahead of time, the few things macOS forbids. Horizon services are re-implemented and the GPU API is translated to Metal.

In this video: how it works in 4 things, Napple Studio (drag a game onto the window), the game on a Mac, on a TV and on iOS (honest status: Simulator only so far), the in-game overlay (render scale, upscaler, amiibo and IR camera simulators), the Mii Maker running from your own firmware, and what works, what's rough and what's next.

LEGAL: You bring your own legally dumped game, your own prod.keys and your own firmware, from a console you own. Napple includes no games, no keys, no firmware. The built app is for your personal use; please don't share builds. Never post keys, firmware or game files anywhere. Not affiliated with or endorsed by Nintendo.

HELP WANTED: document games, write mods and fixes, report bugs with logs, test on more Macs, translate, test on iPhone/iPad.
Repo: https://github.com/erostalevi/Napple
Start here: https://github.com/erostalevi/Napple/blob/main/CONTRIBUTING.md
Good first issues: https://github.com/erostalevi/Napple/issues?q=label%3A%22good+first+issue%22
Docs: https://github.com/erostalevi/Napple/blob/main/docs/index.md
Community: [DISCORD_OR_FORUM_URL]
Sponsor: [SPONSOR_URL]

CHAPTERS
0:00 Four things, not an emulator
0:15 The legal part (please read)
0:55 Why the Switch and your Mac speak the same language
3:00 Horizon and the GPU: the other half
4:30 Napple Studio: drop a game, get an app
6:00 Three screens: Mac, TV, iPhone and iPad
8:30 The overlay: render scale, amiibo, IR camera, Mii Maker
10:00 What works, what's rough, what's next
11:30 How you can help
12:35 Outro

Tested titles: Link's Awakening, Super Mario Odyssey, Animal Crossing: New Horizons, Mario Kart 8 Deluxe (all other games are untested).
Recorded on <Mac model, chip>, macOS <version>, Napple commit <hash>.
Music: <credits and licences>.

#Napple #AppleSilicon #SwitchOnMac #Recompiler #Metal #Programming #OpenSource
```

Tags (YouTube tags field): napple, switch native mac, apple silicon, arm64, aot recompiler, no jit, metal, horizon hle, nvn, macos 26, liquid glass swiftui, open source, solo developer, game preservation, ios no jit.

The chapter timestamps must be re-timed to the final edit; YouTube needs the first chapter at 0:00 and at least three chapters of 10+ seconds.

## Pinned comment

```
The legal bit again, because it matters: Napple has no games, keys or firmware. You bring your own, from a console you own, and the app you build is for personal use. Please don't ask for or post any of those here.

What I need most: (1) document a game you own (docs/titles), (2) report a bug with the log, (3) test on your Mac and tell me the chip and fps, (4) iPhone/iPad testers. Start here: https://github.com/erostalevi/Napple/blob/main/CONTRIBUTING.md

Honest status: 4 games tested, Link's Awakening is the most solid, iOS has only run in the Simulator so far. Ask me anything about how it works.
```

## Shorts / TikTok cut list (5 clips, vertical 9:16, 20-45 s)

Every clip ends with a 2 s card: `Own game. Own keys. Not affiliated with Nintendo.` and the repo URL as a caption.

| # | Hook (first 2 s, spoken + text) | Content | Source shots | Length |
|---|---|---|---|---|
| 1 | "This Switch game runs natively on my Mac. Not emulated." | Cold open footage, then the four-things animation in 4 quick cuts | S02, S03, S05 | 30 s |
| 2 | "Four registers. That's the whole CPU problem." | x18, thread pointer, svc, counters, one line each | S05 | 25 s |
| 3 | "Drag a game. Get an app." | Studio drop, plan, build checklist, done, double-click play | S12-S14, S16 | 35 s |
| 4 | "One F1 key and you have a settings panel over the game." | The overlay tabs, render scale, the amiibo list, the camera tab | S31-S33 | 35 s |
| 5 | "I built the Mii Maker from my own firmware." | The New Mii flow with the live 3D head | S34 | 40 s |

Optional 6th, only if the physical-device gate passes: "Switch game on an iPhone without JIT" (S28-S30).

## X / Twitter thread draft

1. I made a Switch game run as a native Mac app. No emulator, no JIT. Here's how it works, in 4 things 🧵 [video link]
2. The Switch and Apple Silicon are both ARM64, so the game's own code runs directly on the CPU. I only rewrite what macOS forbids, once, ahead of time: reserved register x18, the thread pointer, svc system calls, timer counters.
3. The other half: Horizon (the console OS) is re-implemented as services, and the GPU API (NVN) is translated to Metal. Coverage of the platform is an estimated ~87%, but coverage is not compatibility. [screenshot of the status table]
4. Napple Studio (SwiftUI, Liquid Glass): drop a game, watch the stages, get a signed local .app. [Studio clip]
5. In-game overlay: render scale, upscaler, frame generation, a virtual amiibo and a webcam stand-in for the IR camera. The Mii Maker runs from your own firmware. [overlay clip]
6. Honest status: Link's Awakening plays at 60 fps. Mario Odyssey plays but has visual bugs. Mario Kart 8 races at 40-47 fps. Animal Crossing gets through the opening. Only 4 games tested. iOS: Simulator only so far.
7. Legal: bring your own game, keys and firmware. Napple ships none of them. Not affiliated with Nintendo.
8. It's a one-person project. Want to help? Document a game, report a bug with logs, test on your Mac, write a mod. https://github.com/erostalevi/Napple
