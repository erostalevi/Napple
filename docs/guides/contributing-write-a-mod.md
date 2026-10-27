# Write a mod or a game fix

## Goal
Share a change to how a game runs on Napple (a bug fix, a patch, a cheat, a settings preset) as data, without distributing any game content. Use it with the *Mod or fix submission* issue form.
> **The mod format is documented separately.** See [the mod format](../mods/format.md) and [authoring a mod](../mods/authoring.md); this page covers the rules around it: what is allowed, how to find what to patch, and how to submit.

## Rules
1. **Data only.** A mod contains offsets and replacement bytes, cheat text, settings, or files you wrote. It never contains game executables, extracted assets, keys, firmware or Nintendo code ([legal rules](../../CONTRIBUTING.md#legal-rules-first)).
2. **License and author.** State your license in the submission (MIT, CC0 or similar is easiest). You must have the right to share it.
3. **Target builds exactly.** A patch applies to specific game builds. Name the title id and the build id(s); `napple compat show <titleid>` prints the build id, and the data directory is named after it ([module-format.md](../architecture/module-format.md)).
4. **Say what it does and what it risks.** Code patches can crash a game or corrupt saves: say so, and say which build you tested on.
5. **Prefer fixing Napple.** If the game only needs a mod because Napple has a bug (a missing service, a wrong answer), the right fix is in the runtime, and per-game behaviour belongs in data tables, not code branches. Open a *Bug report* or a pull request instead of, or in addition to, a mod.

## Workflow (format-independent)
1. Reproduce the problem or the wish headless ([document a title](contributing-document-a-title.md) steps 2-3) and note the scene.
2. Find what to change: logs, the `NAPPLE_PCPROF` and crash output name a module and offset (`<module>+0x<offset>`, [debugging.md](debugging.md)). Work out the change with your own tools on your own copy of the game; do not paste disassembly into the issue, give offsets and a description.
3. Write the mod in the documented format (see [format](../mods/format.md)), test it on a fresh `--save-root`, and run the scripted boot to show it still boots: frames, exit 0, no `CRASH`.
4. Submit: either the *Mod or fix submission* form (paste the patch text or link a repository), or a pull request that adds the mod text under the directory the mod docs name.
5. Say the status honestly: *verified* (ran it), *untested*, and the game build.

## What a maintainer will check
No game content; license present; build ids named; the patch text is readable; a boot log or description of the test; no secrets or personal paths.
