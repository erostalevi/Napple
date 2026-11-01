---
name: percent-analysis
description: "%Analysis" of the Napple decompiler - an estimated % of the original Nintendo Switch (NSW) platform that the pipeline and runtime cover (AOT A64 rewrite, loader, Horizon kernel svcs, HLE IPC services, NVN API, Maxwell shader ISA, GPU formats, nvdrv, audio, input, containers/updates/DLC, saves, applets, media), with the change since the last run from the latest feats and fixes, and a ranked list of the missing pieces to implement first. Game-agnostic: it never scores per-game compatibility. Use when the user asks for "%Analysis", "% coverage", "how complete is the decompiler", "what is missing", or "what should we implement next".
---

# %Analysis: Napple decompiler coverage

Estimates how much of the **Switch platform** Napple covers, not how many games run. A title booting is never evidence
by itself; only the generic feature behind it counts.

Script: `.claude/skills/percent-analysis/scan.py` (run with `python3 -I` and an absolute path; it finds the repo itself).
Files next to it: `verified.json` (your per-item judgements, reused by later runs), `history/<date>-<sha>.json` (one schema-2 snapshot per analysis: judged ratios, final %, range, item statuses, priorities).

**Repo gotcha:** the git repo is the OUTER folder `/Users/neto/Documents/Napple`; never run `git` inside `Napple/`.

## Out of scope (never scored, never a priority)
`knownSignatures/`, `compat/matrix.json`, `COMPATIBILITY.md`, `game.json`, per-title patches or notes, Studio, packaging,
launcher, iOS install. Change notes mention games: take the **generic** gap they reveal ("SDK media library H.264
decoder not replaced"), never the title ("SMO tutorial movies").

## 1. Measure

```bash
python3 -I /Users/neto/Documents/Napple/.claude/skills/percent-analysis/scan.py
```
It counts, against fixed reference catalogs of the Switch (tiers core 3 / common 2 / rare 1; impl 1, partial 0.75,
stub 0.5, missing 0):
- **exact** domains: rewriter site kinds (`scan.rs`), svcs (`svc_register` vs the application-reachable Horizon svcs),
  service names (string literals in `runtime/src/services`), `IPC_IMPL` vs `IPC_STUB` command rows, `NVN(...)`
  definitions vs `gfx/src/nvnapi/names.txt`, Maxwell mnemonics named in `maxwell_translate.c` comments;
- **probe** domains (marked "(probe)"): keyword hits outside comments and tests. A hit is a lead, not proof.

It also prints commits and `docs/archive/changes/` open items (Not done / Risks / pending) since the previous run, and the delta
per domain. Domain weights (sum 100) are in `DOMAINS` in the script; change them only if the user asks.

## 2. Verify (this is the analysis; do not skip)
- Every line under **Unverified probe items**: open the hit (and the code around it) and decide impl / partial / stub /
  missing. Partial = works for the common path but a documented branch fails or is refused. 0 hits: grep under other
  names before calling it missing.
- Exact domains can also be wrong. Spot-check every **core** item listed missing (e.g. a shader op handled under a
  different comment, an svc routed by another table) and the top of `missing_by_object` for NVN. Also check implemented
  items that are stubs in disguise: NVN functions that only store a value Metal never reads, svcs that "succeed" as a no-op
  where the guest relies on the effect (check `docs/architecture/runtime-services.md`).
- Record each judgement in `verified.json`: `{"<domain>/<item name exactly as printed>": {"status": "partial",
  "note": "file:line - why", "date": "YYYY-MM-DD"}}`. Use parallel Explore subagents for this only if there are more than
  ~25 items to check.
- Re-check an existing entry when `git log --since=<its date> -- <file>` shows its file changed.

## 3. Weigh the latest feats and fixes
Read the commits and open items the scan printed, plus `git status`/`git diff --stat` (uncommitted work is the newest).
Adjust a domain's ratio by at most **±15 points** and write the reason in one line. Rules:
- Work labelled "proposed", "untested", "needs a Mac" counts as partial until verified by a test or a run.
- A known crash/hang/corruption in a generic path (e.g. dirty tracking, pipeline build) lowers its domain until fixed.
- A fix with a regression test raises confidence, not coverage: coverage only rises when a gap closes.
- Correctness limits the design accepts (cooperative scheduling without time slices, transfer memory as no-op, offline
  network stubs) are partial, not impl.

## 4. Judgement file and final %
The final number is **computed by the script, never by hand**, so two runs are comparable. After steps 2-3 and 5, write a judgement file in the scratchpad (not in the repo):
```json
{"adjustments": {"<domain key>": {"delta": -0.05, "reason": "one line, with file:line or commit"}},
 "priorities": [{"item": "<item name exactly as printed>", "domain": "<domain key>", "importance": 3, "effort": "S",
                 "why": "why it matters on the Switch in general", "where": "file/module"}],
 "notes": "optional: judgement calls"}
```
`delta` is a ratio change for that domain (clamped to ±0.15; the ±15-point rule of step 3). Only domains you adjusted need an entry.
Then save the snapshot (this is the baseline for the next run; save once per analysis):
```bash
python3 -I /Users/neto/Documents/Napple/.claude/skills/percent-analysis/scan.py --save --judgement <file>
```
It computes `final = Σ weight × judged ratio`, the range (± half the weight of the probe items still unverified), the ranked priorities (`importance × weight ÷ effort`, S=1 M=3 L=8) with
each item's `gain_pts` (the coverage points closing it adds), and writes `history/<date>-<sha>.json`. Use `--scoreboard` instead of `--save` to preview without writing.

## 5. Priorities
Rank every missing/stub/partial item by `importance × domain weight ÷ effort` (the script does the arithmetic from your importance/effort picks; include at least 15 items so `/napple-plan` can choose):
- importance: 3 = the SDK or most games hit it at boot or every frame (a missing one stops the runtime), 2 = common
  feature class (online-off fallbacks, video, applets, half-precision shader ops, compute atomics), 1 = rare;
- effort: S (hours, table entry or stub with safe default), M (days), L (weeks, new subsystem);
- prefer gaps that fail loudly for many titles (the runtime stops on missing IPC/svc, the shader translator `fail()`s)
  over gaps that only degrade output.
Use general Switch knowledge (Horizon, NVN, Maxwell, audren) to set importance, not the two games tested so far.
Items must be catalog items as printed (so `gain_pts` is known); a gap that is not in a catalog (a design limit such as "no time slices") goes in with a descriptive name and `gain_pts` n/a.

## 6. Report (standardized: always exactly this, in chat)
The report is the output of `scan.py --save` (or `--scoreboard`), pasted as is, followed by at most 5 lines of commentary. Never reformat it, reorder it or rename columns: the fixed shape is what makes runs comparable.
Sections, in order: title line, `SCORE ...` machine line, the headline, `## Domains` (every domain, fixed order, Measured / Adj / Judged / Points / Delta / Note), `## Priorities` (top 10), `## Since last run`, `## Confidence`.

## 7. Comparing runs
```bash
python3 -I /Users/neto/Documents/Napple/.claude/skills/percent-analysis/scan.py --compare            # last two snapshots
python3 -I /Users/neto/Documents/Napple/.claude/skills/percent-analysis/scan.py --compare <old> [<new>]   # file name or date
```
Prints `CHANGE final A -> B`, per-domain points, gaps closed/improved, regressions, new catalog items, unverified and TODO counts, and priorities closed or new. The `SCORE` line of every snapshot's report is greppable.
Offer an Artifact page only if the user wants to share it.
