# 2026-10-27 youtube-script: a shootable YouTube video kit (docs only)

Branch `claude/youtube-script-2026-10-27`, Linux cloud session, batch 2026-10-27-polish. No code touched; owns `docs/media/` (plus one link line in `docs/index.md` and one row here, both required by `tools/check_docs.py`).

## What was written (`docs/media/youtube/`)
- `script.md`: 12:30 shooting script. Hook: "I didn't emulate the Switch. I changed four things" (x18, thread pointer, svc, timer counters). Cold open, legal part at 0:15, seven chapters with timestamps, spoken lines, VO, on-screen text, the three-device chapter (Mac, TV, iPhone/iPad), a "works / rough / next" segment, music and sfx guidance (royalty-free only), four humor beats (all self-directed), and a source table that ties every claim to a repo file.
- `cta.md`: nine asks with repo-relative start points, link placeholders, outro card layout, good-first-issue and labels proposal, community channel proposal.
- `shotlist.md`: 37 numbered shots plus A-roll with device, game, scene, length, capture settings and the scripted trigger (`NAPPLE_INPUT_SCRIPT`, `NAPPLE_SCREENSHOT`, `NAPPLE_FE_OVERLAY`, `NAPPLE_AMIIBO`, `NAPPLE_IR_SOURCE`, `NAPPLE_SWKBD_ANSWER`, `napple build --firmware-program`), recording checklist, pre-flight checklist.
- `titles-thumbnails-description.md`: 10 titles, 5 thumbnails, description with chapters, legal text and tags, pinned comment, 5 Shorts, X thread.
- `README.md`: one-page index.

## Decisions (taken without asking)
- **iOS is shown honestly.** The repo says nothing has run on a physical iPhone or iPad (Simulator only), and iOS is out of scope for this batch. The script has Version B (default: Simulator footage, "not yet", an invitation to testers) and Version A (real device) used only if the pre-flight gate passes. The decision is made the evening before the shoot.
- **Coming features are never demonstrated.** Mods (`.napmod`), custom render scale, widescreen/portrait output and the own upscaler are marked "coming"; the overlay already offers 0.5x-4x, supersampling, an upscaler and frame generation, so only those are shown.
- **Hero title is Link's Awakening** (60 fps live). SMO is B-roll with a spoken caveat; MK8D and ACNH appear in the status table, not as showcases (45 fps ceiling, stopgap picture).
- **Repo name** uses `github.com/erostalevi/Napple` as the brief says; the cloud mirror is `NappleCloudRepo`, so the name is a placeholder to replace before publishing (noted in `cta.md`).
- Humor beat 4 has a safe alternative in case it reads as mocking anyone.
- Mii faces on MK8D racers are NOT claimed (the Mii note says they were not confirmed).

## Status
Documentation only: nothing here was run. Every factual statement is traceable via the source table at the end of `script.md`. `python3 tools/check_docs.py` passes (124 files). Nothing was published outside the repo.
Remaining Mac/creator steps: shoot the footage, verify the TV and fullscreen take, decide Version A or B for iOS, create the labels and starter issues, set the sponsor and community links, re-time chapters to the final edit.
