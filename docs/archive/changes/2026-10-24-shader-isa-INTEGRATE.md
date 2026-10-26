# Integration notes: shader ISA round (`claude/adoring-dirac-ng6iif`)

For whoever merges this branch into `main`. Read `2026-10-24-shader-isa.md` (same folder) for what the round adds; this file only covers how to land it.

## Branch
- Branch: `refs/heads/claude/adoring-dirac-ng6iif`, pushed to `origin`. Commits on top of `main` (1f4b902):
  - `9501a91` shader: LDS/STS, TLD, TXD, atomics, VOTE, CS2R and fewer fail() paths (cloud session, Linux, untested on Metal)
  - `ebdbe59` shader: Metal-verify the ISA round; TLD 1D lod fix; corpus re-run (this Mac)
  - plus the commit that adds this file (docs only)
- Local worktree: `.claude/worktrees/adoring-dirac` (can be removed after the merge).
- Already verified on this Mac (Apple M5): `make -C Napple/gfx test` passes, including 41 Metal compile checks in `t_xlate`.

## Files touched
- `Napple/gfx/src/shader/maxwell_translate.c`, `Napple/gfx/src/shader/maxwell.h`
- `Napple/gfx/tests/t_xlate.m`, `Napple/gfx/tests/t_xlate_linux.sh` (new)
- `Napple/gfx/src/mtl_shader.m`: one line, `TRANSLATOR_VERSION 6u`
- `Napple/docs/changes/2026-10-24-shader-isa.md`, this file

## Merge order and conflicts
- `main` is an ancestor of this branch: it fast-forwards onto current `main`.
- Of the other open fronts, only `nvn-pipeline-2026-10-24` touches gfx, and it edits `mtl_shader.m`. `git merge-tree` shows a **clean** merge with it in either order. Its `TRANSLATOR_VERSION` is still `4u`, so after both are merged check that the line reads **6u** (or higher, if nvn-pipeline also changes generated MSL: then bump once more). Never let it fall back to 4 or 5. Stale disk caches would load old MSL.
- `shader-isa`, `worktree-loader-kernel` and `audio-fs-2026-10-24` don't touch gfx.
- `ShaderKey` gained a field at the end (`shared_size`). Anything that hashes or memcmps `ShaderKey` (pipeline cache, prewarm list) is invalidated by the version bump. nvn-pipeline doesn't touch `maxwell.h` today. If a later version of it adds a `ShaderKey` field, keep both fields, append rather than reorder, and bump the version.

## Steps
```bash
git checkout main
git merge --no-ff refs/heads/claude/adoring-dirac-ng6iif
make -C Napple/gfx clean test
```
Then the corpus checks (each must print `N shaders, N translated, N compile with Metal` and `t_xlate: ok`):
```bash
NAPPLE_SHADER_CORPUS=$PWD/Napple/work/shaders/t_las Napple/gfx/build/tests/t_xlate | grep -E "FAIL|corpus|t_xlate"
```
```bash
NAPPLE_SHADER_CORPUS=$PWD/Napple/work/shaders/t_smo Napple/gfx/build/tests/t_xlate | grep -E "FAIL|corpus|t_xlate"
```
Expected: LAS 679/679/679, SMO 128/128/128, no FAIL lines. Repeat both after merging `nvn-pipeline-2026-10-24` too, then build with napple-latest.

## Known, not a regression
- `sh Napple/gfx/tests/t_xlate_linux.sh` fails on macOS (it compiles `t_xlate.m` as plain C and the Apple headers pull in Foundation). It's for Linux hosts only. Don't fix it on the Mac.
- Changed behaviour for existing shaders: PRMT default mode now honours selector bit 3 (sign replication). The corpora still compile; a visual regression in a game that uses PRMT sign modes would point here.
- The new ops (TLD, TXD, ATOMS/ATOM/RED, VOTE, CS2R, gather offsets) use bit layouts **from memory**. None of them occurs in the LAS or SMO corpora, so nothing real has checked them. If a new title shows wrong texels or wrong compute results, dump the word and check it against the layout comments in `maxwell_translate.c` first.
- LEA and IMAD are not implemented (no real words in the corpora). They fail cleanly as "unknown instruction".

## Do not
- Don't push `main` or delete the branch without the user's go-ahead.
- Don't edit `gfx/src/shader/` while merging beyond conflict resolution. Translator changes belong in their own branch with the corpus re-run.
