# Napple video: call to action

Used in the script as chapter 7 (spoken ask + animated list, [script.md](script.md)) and as the outro card (S36, S37 in [shotlist.md](shotlist.md)).
Principle: one specific thing a stranger can do in 30 minutes, with a link, and the legal rule repeated. Nothing proprietary is ever requested from viewers.

## Link placeholders

Replace before publishing. Use the real names; these are the placeholders the video and description use.

| What | Placeholder |
|---|---|
| Repository | `https://github.com/erostalevi/Napple` |
| Where to start (proposal until the labels exist) | `https://github.com/erostalevi/Napple/issues?q=is%3Aopen+label%3A%22good+first+issue%22` |
| Contributing guide | `https://github.com/erostalevi/Napple/blob/main/CONTRIBUTING.md` |
| Documentation | `https://github.com/erostalevi/Napple/blob/main/docs/index.md` |
| Compatibility / support table | `https://github.com/erostalevi/Napple/blob/main/docs/reference/support-matrix.md` |
| Discord or forum | `[DISCORD_OR_FORUM_URL]` (proposal: GitHub Discussions first, a Discord only if the maintainer wants one) |
| Sponsor | `[SPONSOR_URL]` (GitHub Sponsors or similar, set up before publishing) |

Note for the maintainer: the cloud mirror of this repository is currently named `NappleCloudRepo` (the README badge points there). Pick the final public name before the video goes out and search-replace the placeholder.

## What to ask for (nine items, with where to start)

| # | Ask | Who it suits | Where to start (links are repo-relative) | Proposed first task |
|---|---|---|---|---|
| 1 | **Document a title** | Anyone who owns a game, can run a command and write | The four reference notes in [../../titles/README.md](../../titles/README.md), the generated [support matrix](../../reference/support-matrix.md), the guide [add game support](../../guides/add-game-support.md) | Run the scripted boot for one more game you own, write what happened (stage reached, evidence, missing services) in the compatibility report form |
| 2 | **Mods and fixes** | Hex-editing and patching people, cheat authors | `.napmod` format and `docs/mods` (not shipped yet: the format is being written; say "coming" until it lands) | Write a small fix as a pchtxt or IPS patch for a game you own and keep it ready for the format |
| 3 | **Bug reports with logs** | Everyone | The bug report form in `.github/ISSUE_TEMPLATE`, [debugging guide](../../guides/debugging.md), [symptoms](../../symptoms.md) | File one report with the Napple commit, Mac model, macOS version and the log; never attach game files, keys or firmware |
| 4 | **Graphics goldens** | Graphics-minded testers | [testing guide](../../guides/testing.md) and the golden-frame harness once documented there | Capture reference frames of a scripted route on your Mac and compare |
| 5 | **Translations** | Bilingual people | Studio strings go through localisation (`LocalizedStringKey`); docs translations | Translate the Studio strings or the README into your language |
| 6 | **Test on more Macs and chips** | Owners of M1/M2/M3/M4, base-model and older Macs | [CONTRIBUTING.md](../../../CONTRIBUTING.md), the performance logging switch `NAPPLE_PERF_LOG=1` in [environment variables](../../reference/environment-variables.md) | Run the headless 45 s boot on your chip and post the fps line |
| 7 | **iPhone and iPad testing** | Brave people with a free Apple ID | [iOS guide](../../architecture/ios.md) (device path written, never run on hardware) | Follow the signing steps and report exactly where it stops |
| 8 | **Studio UI feedback** | Designers, accessibility reviewers | [Studio guide](../../guides/studio.md) | Screenshots and notes: VoiceOver, keyboard navigation, dark/light mode |
| 9 | **Star or sponsor** | Everyone | the repo and the sponsor link | Click the star; sponsor if you can; share the video |

## Legal line (always attached to the ask)

"Only with your own games, keys and firmware. Never attach game files, keys, firmware or dumps to an issue, a pull request or a Discord message. Napple is not affiliated with Nintendo."
(README "Legal, up front" and CONTRIBUTING.md carry the same rules.)

## Spoken version (60-75 seconds)

Already written into [script.md](script.md) chapter 7. Short cut for a Short/TikTok: 20 seconds, "I'm building a Switch-to-Mac native recompiler on my own. I need people to document games, test on different Macs and report bugs with logs. Link in bio. Bring your own games, keys and firmware."

## Outro card (S37)

Layout (16:9):
- Left: repo name and URL large, `Star the repo` with a small star icon; a QR code of the repo URL (generate it, do not use a third-party QR service).
- Center: three lines: `Document a title · Mod or fix · Report a bug`.
- Right: the YouTube end-screen slots (subscribe button, one suggested video).
- Footer, small: `Bring your own game, keys and firmware. Not affiliated with Nintendo.`
- Colors: dark graphite background, one accent (the same accent as the thumbnails), white text, no game art.
Duration 20 s, last 5 s clean for the end-screen elements.

## Good-first-issue list (proposal for the maintainer, not created by this video)

Labels (to be created in the repo, see the contributor kit in the same batch): `good first issue`, `help wanted`, `catalogue`, `mod`, `graphics`, `docs`, `translation`, `ios`, `perf`, `studio`, `needs-log`.
Starter issues to open before publishing (each small, each linked from the description):
1. "Document one more title: run the scripted boot and fill the compatibility report"
2. "Add the missing support-matrix evidence for a title already in the matrix"
3. "Translate the Studio strings into <language>"
4. "Run the 45 s headless boot on your chip and post the fps line" (one issue, many comments)
5. "iPhone or iPad: follow the iOS guide and report where it stops"
6. "Check the docs for a broken link or an unclear step"
7. "Write one pchtxt fix for a synthetic test title (practice for the mod format)"

## Community channel (proposal)

Start with GitHub Discussions (no extra tool, tied to the repo, searchable). Add a Discord later only if volume demands it. Put the code of conduct link in the welcome post, a pinned "Start here" thread, and a rule: no requests for games, keys or firmware, ever (such posts are deleted and the author is warned).
