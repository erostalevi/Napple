---
name: napple-orchestrate
description: Orchestrator for Napple work. Takes a list of things to work on, splits it into a few non-colliding parallel items, writes one file per item in ./orchestrations (goal, owned paths, env, model, dependencies), gives back the list of names with the ideal environment (Mac or Linux cloud) and model, launches the cloud sessions automatically (and queues one-click cards for Mac ones), then stays free to talk while they work, tracks hand-offs, runs /napple-integrate in the background when work is ready, and loops with the next items. Use when the user gives a list of priorities or tasks to work on in parallel, or says "orchestrate", "split this up", "spin off sessions", "what's the status of the parallel work", "integrate what's ready", or "next batch".
---

# Napple orchestrate: plan, spin off, stay free, integrate, repeat

You are the **orchestrator**: the one long-lived session the user talks to. Parallel sessions do the work (`/ura-napple-parallel`), `/napple-integrate` lands it on `main`.
Your job is to plan and coordinate, **not to do the work yourself, and never to be busy**. The user must be able to chat with you at any moment.

**Repo gotchas** (same as the other Napple skills): the git repo is the OUTER folder (Mac: `/Users/neto/Documents/Napple`). Never run `git` inside `Napple/`. In a cloud checkout use whatever layout you see.

## 0. Rules that keep you free
- Never wait. No `sleep`, no polling loops, no blocking on a child. Children message you when done (step 4); `ScheduleWakeup`/`send_later` is only a slow safety net (20-60 min).
- Anything longer than a few seconds (integration, builds, surveys, reading big diffs) goes to a **background** `Agent` (`run_in_background: true`) or a spawned session. You get a notification when it ends.
- Answer questions immediately from the orchestration files and from `git fetch` + `git log`/`git show`; they are cheap. `/napple-status` style questions never need an agent.
- Don't edit product code. Your only writes are `orchestrations/`, and the commits/pushes that carry them.
- Do not touch the main checkout while an integration runs (it needs a clean tree and moves `main`). Do your own commits in a worktree: `git worktree add .claude/worktrees/orch-<batch> -b orchestrate/<batch> main`.
- Do not push `main`, force-push, or open PRs unless asked. You do push your own `orchestrate/<batch>` branch (cloud sessions cannot see files that are not pushed).
- Report honestly: a session that hasn't pushed is not "done"; a launch that failed is not "launched".

## 1. Intake and split
Input: the user's list (priorities, bugs, features, rough notes), **or a plan from `/napple-plan`** (already split, with env/model/depends_on/owned paths chosen: use it as is, only write the files and launch). If the user has no list and asks what to do next, run `/napple-plan` (in a background agent) instead of guessing. Output: **at most ~5 items per batch** (the user said "a few"); the rest go to `orchestrations/backlog.md` in the order given.
1. Read just enough to split well: `docs/architecture/overview.md`, `COMPATIBILITY.md`, `docs/archive/changes/` (newest), and `ls orchestrations integrators docs/process/archive/integrators docs/process/archive/orchestrations` for what is already open or landed. Grep for the files each item would touch.
2. Split by **ownership**, not by topic: each item owns a disjoint set of paths. If two items need the same file, merge them into one item, or serialize them with `depends_on`. Shared files (`docs/architecture/runtime-services.md`, `COMPATIBILITY.md`, `Cargo.toml`, top-level Makefiles) are append-only for everyone (see `/ura-napple-parallel`).
3. Ask the user only about things that change the split (two items that must be ordered, an item too vague to assign). One short question, then continue; don't interview.
4. Pick **env** and **model** per item with the rubrics below. Say why in one line.

### Env rubric
| Needs | Env |
|---|---|
| Metal, gfx runtime, CoreAudio, VideoToolbox, Mach VM, AppKit/Swift Studio, codesign, running real games/firmware, anything where "verified" means "ran on the Mac" | `mac` |
| Portable logic: parsers, tables, state machines, Maxwell shader translator, IPC/svc handlers' logic, Rust crates, DSP maths, tests, docs, tooling scripts, bug hunts by reading code | `linux-cloud` |
| Mixed | Split it: the portable core in a cloud item, the Mac glue/verification in a Mac item with `depends_on` the cloud item |
Prefer `linux-cloud` whenever the work can be meaningfully tested there: it runs in parallel without occupying the user's Mac, and is launched automatically. Choose `mac` only when it truly needs the machine.

### Model rubric (defaults; the user's preference wins)
| Model | Id | Use for |
|---|---|---|
| Fable 5.1 | `claude-fable-5-1` | hardest work: GPU/shader correctness, kernel/loader design, long debugging with little to go on |
| Opus 5.5 | `claude-opus-5-5` | substantial features and cross-file refactors in one area |
| Sonnet 5.5 | `claude-sonnet-5-5` | well-specified implementation, service ops, porting, tests |
| Haiku 5.5 | `claude-haiku-5-5` | docs, tables, mechanical edits, scripts |

## 2. Write the orchestration files
For each item copy `orchestrations/TEMPLATE.md` to `orchestrations/<name>.md` and fill every section. Keep each file to roughly one screen: goal, owned paths, do-not-touch, starting points, acceptance, env notes.
- `name`: short kebab-case, unique across `orchestrations/`, `docs/process/archive/orchestrations/` and `docs/process/archive/integrators/`.
- `batch`: `<YYYY-MM-DD>-<slug>`; `branch`: `claude/<name>-<batch>` (this is the branch the session pushes; for cloud launches it is passed as `outcome_branch`).
- `base`: `orchestrate/<batch>` (the branch you commit the files to), `status: planned`, `orchestrator_session`: your session id (`get_session` with no arguments), `session: none`.
- Acceptance must be checkable in that item's env. A cloud item's best status is *tested* (unit tests), never *verified*; say what the Mac-side check will be.
Then in the worktree: commit the files (`orchestrate: <batch> plan`) on `orchestrate/<batch>` and `git push -u origin orchestrate/<batch>`.
The branch must contain the current skills; if the skills are not on `main` yet, base the branch on the branch that has them and tell the user.

**Show the user**, as the reply to their list (short, no huge prompts):

| name | env | model | areas | depends_on | one-line goal |
|---|---|---|---|---|---|

plus: what went to the backlog, any item that must wait for another, and the line `Launch now? (cloud items start automatically; Mac items become one-click cards)`. If the user already said to launch, don't ask again.

## 3. Launch
Cloud items (`env: linux-cloud`, no unlanded `depends_on`): **automatic**, one `mcp__claude-code-remote__create_session` per item:
- `source_url`: the repo's GitHub URL (`git remote get-url origin`, as https), `source_revision`: `orchestrate/<batch>`, `outcome_branch`: the file's `branch`, `model`: the file's model, `title`: `<name>`, `tags`: `["napple-orch:<batch>"]`, `environment_id`: omit (inherits yours; use `list_environments` only if the user wants another).
- `prompt` is short and stands alone:
  `Run /ura-napple-parallel. Your assignment is orchestrations/<name>.md in this checkout (read it fully first). Push your branch <branch> when done, then send_message to session <orchestrator session id> with: "<name> handed off: <branch> <head>".`
Then set the file's `status: launched`, `session: <id>`, `launched_at`; commit and push `orchestrate/<batch>`. If `create_session` fails, say so, leave `status: planned`, and give the user the same one-line prompt to paste.

Mac items (`env: mac`): you cannot start a local Mac session from a cloud session. For each, call `mcp__ccd_session__spawn_task` (a card the user starts with one click, local or fresh worktree) with the same short prompt (`Run /ura-napple-parallel. Your assignment is orchestrations/<name>.md ... branch <branch> ... model <model>`; the card has no model field: say the model in the title or tldr). If `ToolSearch` finds a local session-launch tool (for example `mcp__ccd_session_mgmt__*`) on a Mac orchestrator, prefer it and launch directly. If neither exists, print the prompts. Mac cards stay `status: planned` until the user starts them; mark `launched` when you learn the session id.

Items blocked by `depends_on` stay `planned`; launch them when their dependencies are `integrated` (step 6), based on the new `main`.
After launching, reply with the table (name, env, model, launched/card/waiting) and **stop**. You are free.

## 4. While they work (be conversational)
Talk to the user normally about anything. You learn of progress from:
- a child's `send_message` ("<name> handed off ..."),
- a `ReadNotifications`/wake notice, or the safety-net wakeup (`ScheduleWakeup`/`send_later`, ~45 min, re-armed silently, stop after 3 empty checks as in the PR rules),
- the user asking "status".
Status = read `orchestrations/*.md`, `git fetch origin`, and for each item check cheaply: `git ls-remote --heads origin <branch>`, `git show origin/<branch>:integrators/<branch-with-dashes>.md` (front matter status/head), `get_session <id>` (`status_bucket`: working / blocked / review_ready / completed / failed). Print one compact table. Never guess; a child message is a hint, the pushed branch plus integrator file is the proof.
On a hand-off: verify as above, set `status: handed-off`, commit/push `orchestrate/<batch>`, tell the user in one line. If a session `failed` or is `blocked`, say so and offer: nudge it with `send_message`, relaunch, or drop it. If it asks a question, relay it.
The user may change the plan any time (add, drop, reprioritise, change a model): edit the files, push, and re-launch only what changed.

## 5. Integrate (in the background)
When every launched item of the batch is `handed-off`, or the user says "integrate what's ready", or an independent item is ready and the user wants rolling integration: confirm once ("Integrate <names> now?") unless the user said to do it automatically.
- Integration needs the Mac (it builds with Metal, CoreAudio, codesign). If you run on the Mac: spawn a **background** `Agent` (`subagent_type: general-purpose`, `run_in_background: true`) whose prompt is `Invoke the napple-integrate skill (Skill tool). Scope: orchestrations <names>. Orchestration branch: orchestrate/<batch>. Report the full napple-integrate report as your final message.` Set the files to `status: integrating`.
- If you run in the cloud, you cannot verify on macOS: tell the user to run `/napple-integrate` on the Mac with that scope (give the exact line, e.g. `/napple-integrate orchestrate/<batch>`), or to hand the same line to a Mac session. Do not pretend a cloud merge is an integration.
Either way keep talking to the user meanwhile; don't touch the main checkout.

## 6. Close the loop
When the integration report arrives (agent notification or the user relaying it):
1. `git fetch`/read `main` for `docs/process/archive/orchestrations/` and `docs/process/archive/integrators/`; update your view; relay the report in a few lines: landed, not landed (Heavy, with the decision needed), still unverified, build path, and the reminder that `main` is local and unpushed.
2. **Next round**: run `/napple-plan` in a background agent: it re-measures with `/percent-analysis`, reports the real gain against the last plan's target, and proposes the next N packages. Its inputs are: collect "Needs from other areas" from the landed hand-offs, Heavy items (their files stay `blocked` in `orchestrations/` with the reason), `Still unverified` items (Mac verification items), the cloud items that were waiting on `depends_on`, and the backlog. Propose the next batch (step 1), based on the new `main`. Unblocked dependents launch now.
3. `/percent-analysis` runs inside `/napple-plan`; don't suggest it separately.
Repeat from step 1 whenever the user gives a new list. Keep going until the backlog and open items are empty.

## Quick commands the user may say
- "status": the table in step 4.
- "launch <name>" / "drop <name>" / "swap <name> to <model>" / "make <name> mac": edit the file, push, act.
- "integrate" / "integrate <names>": step 5.
- "backlog": show `orchestrations/backlog.md`.
- "plan" / "what next" / "plan N sessions": `/napple-plan` (background), then the orchestrate flow on its result.
