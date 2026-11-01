---
name: napple-plan
description: Decide what to build next in Napple, aiming at 100% Switch-platform coverage with the 80/20 rule. Runs /percent-analysis (or reuses a fresh snapshot), subtracts work already in flight, ranks the gaps by coverage gain per effort, packs them into N parallel work packages (default 5 sessions) with disjoint ownership, shows the expected gain and the road to 100%, and hands the packages to /napple-orchestrate. Use when the user asks "what should we build next", "plan the next round", "plan the sessions", "how do we get to 100%", or gives a number of sessions to play with.
---

# Napple plan: from %Analysis to a batch of parallel sessions

Output of this skill is a **plan**: N work packages ready for `/napple-orchestrate`. It changes no product code and starts no sessions by itself.
Argument (optional): number of sessions (default **5**), a focus domain, or "mac only"/"cloud only". The goal is always the same: close the most coverage points per session, until 100%.

If you are the orchestrator, run this skill in a **background** agent (it reads a lot of code), so the user can keep talking to you; relay the plan when it ends.
**Repo gotcha:** the git repo is the OUTER folder (Mac: `/Users/neto/Documents/Napple`); never run `git` inside `Napple/`.

## 1. Get a current analysis
- `ls .claude/skills/percent-analysis/history`; read the newest snapshot's `commit`. If it equals `git rev-parse --short HEAD` of `main` (or the base you plan on) and the tree is clean, **reuse it**: print `scan.py --compare` for context only.
- Otherwise invoke the `percent-analysis` skill in full (verify probes, judgement file, `--save`). Do not shortcut the verification: the plan is only as good as its priorities list.
- Take from the snapshot: `final_percent`, `range`, per-domain judged points and weights, `priorities` (ranked, with `gain_pts`, importance, effort, where).

## 2. Subtract what is already moving
Read `orchestrations/*.md` (statuses planned/launched/handed-off/integrating/blocked), `orchestrations/backlog.md`, open branches' `integrators/*.md` (`git fetch`, `git branch -r`), and the "Needs from other areas" of `docs/process/archive/integrators/` and landed hand-offs.
- A gap covered by an item that is `launched` or `handed-off` is **in flight**: exclude it, list it as "in flight (name)". Count its expected gain as pending, not as done.
- A `blocked` item (Heavy leftovers) is a candidate again: decide whether it needs a different model/env or a split.
- Handed-off work not yet verified on the Mac: the verification itself is a candidate item (coverage rises only when verified).

## 3. Rank with the 80/20 rule
Score each remaining gap: **gain per effort** = `gain_pts ÷ effort (S=1, M=3, L=8)`, with importance as tie-breaker/weight (a gap that stops the runtime for many titles beats a cosmetic one with equal gain). Then:
1. Sort descending, compute the cumulative gain curve, and mark the **knee**: the smallest set of items that captures ~80% of the achievable gain in this round. These are the first picks.
2. Always include cheap core gaps (S effort, importance 3) even when their gain is small: they unblock the rest.
3. Do not chase rare/never-hit items before common ones, except as filler for a session that would otherwise be idle (haiku/sonnet-sized packages).
4. Items with `gain_pts` n/a (design limits like missing time slices) are valued by importance alone; place them by the importance rule of `/percent-analysis` step 5.
5. Respect the **Road to 100%**: note which gaps may be unreachable on the Mac/Metal (hardware features Metal lacks) or out of scope (see percent-analysis "Out of scope"), so 100% is defined honestly: say "100% of the reachable catalog is X%".

## 4. Pack into N packages
One package = one session = one owner of a set of paths. Rules:
- Packages must have **disjoint ownership** (see `/napple-orchestrate` step 1): group gaps by module (`gfx/src/shader/`, `runtime/src/services/`, `crates/napple-rewrite/`, `audio/`, ...), never split one file between packages, serialize with `depends_on` where one needs another's output.
- Size each package to about one session of work: sum of efforts roughly equal across packages (about 1-2 M, or 1 L, or several S), the biggest-gain packages first.
- Choose **env and model** with the rubrics in `/napple-orchestrate` (portable logic and tests = linux-cloud, Metal/CoreAudio/verification = mac). Prefer cloud for the bulk; reserve one Mac slot for verifying handed-off Mac-only work when something is waiting for it.
- If fewer than N independent packages exist, plan fewer: never pad with fake work.
- Leftover ranked gaps become the backlog, in order.

## 5. Present (short; this is the reply)
```
# Napple plan - <date> - from %Analysis <final>% (range LL-HH%), commit <sha>
Round target: +X.X pts (to ~YY.Y%)  |  reachable ceiling: ZZ%  |  in flight: +W.W pts pending
| # | name | env | model | areas | closes (items) | gain pts | effort | depends_on |
Cumulative: pkg1 +a, pkg1-2 +b, ... (80% knee at package k)
## Backlog (next rounds, in order)  - one line per package
## In flight (excluded)  - name: what it covers
## Road to 100%: rounds left at this pace, the biggest remaining domains, anything unreachable/out of scope
```
Numbers must come from the snapshot (`gain_pts`, domain points); never invent gain. Then ask once: **"Run it? (hands these N packages to /napple-orchestrate)"**.

## 6. Hand off
On yes (or if the user said to just go): invoke `napple-orchestrate` with these packages as its list, in the same order, with the env/model/depends_on you chose, the owned paths, and each package's
closing items as Acceptance (checkable in that env: a cloud package is *tested*, never *verified*). The backlog goes to `orchestrations/backlog.md`. Do not re-split; do not re-ask what the plan decided.
After the batch is integrated, the next `/napple-plan` run compares against the saved snapshot (`scan.py --compare`) and reports the real gain against the plan's target: put that line at the top of the next plan.
