---
branch: claude/sched-applets-2026-11-01-r1
orchestration: sched-applets
base: df2f318
head: 5713fc3
env: mac
date: 2026-11-01
areas: [runtime/kernel (threads.c), runtime/services/applet*, tools/soak, docs]
status: ready-with-caveats
---

# claude/sched-applets-2026-11-01-r1: Horizon scheduling fixes, swkbd + offline web on real titles (ACNH race still open)

## Summary
- Scheduler (`runtime/src/threads.c`): two real divergences from Horizon fixed and unit-tested. (1) Equal-priority threads rotate every 10 ms only
  at priority 59 (Horizon's preemption priority on the application cores), not at every priority. (2) A thread preempted by a better one keeps the
  head of its priority instead of queueing behind equal-priority waiters. Also: the main thread starts pinned to its npdm ideal core. Plus debug aids.
- **Acceptance 2 is NOT met.** The ACNH base command-buffer overlap / guest null read is not caused by scheduling: it happens about as often with the
  base-commit scheduler, with the new one, and with timer preemption switched off entirely (`NAPPLE_SLICE_MS=0`). The investigation narrowed it to
  the game's job system: guest main thread 0x51 against worker 0x57/0x58 in the same parallel-for job path. The real join of the main thread with the
  workers was not found. Root-cause notes and next steps are in the change note and `docs/todo.md`.
- swkbd verified on Link's Awakening (normal mode, new-file name; scripted answer returned and read back by the game). ACNH's name entry is the
  game's own keyboard, not swkbd. The inline keyboard stays tested-only (none of the four titles uses it).
- Offline web verified: real HtmlDocument layout resolved (`html-document/` prefix), and SMO's manual opened the HOME-menu way in the web sheet
  (`NAPPLE_OPEN_MANUAL`). No title opens the offline applet on its own.
- `tools/soak/soak.py`: `--lockf`, per-run CBTRACE anomaly counts, whole logs kept for runs with anomalies.

## Files
`git diff --stat df2f318..HEAD`:
- `runtime/src/threads.c` (scheduler rules, stats, knobs)
- `runtime/tests/host/test_threads_preempt.c` (P2 changed, P5/P6 new, P3 waiter target)
- `runtime/src/services/applet.c` (NAPPLE_OPEN_MANUAL thread, swkbd verbose config dump)
- `runtime/src/services/applet_web_core.{c,h}` (html-document resolve, web_manual_path, web_manual_lang, applet_web_open_manual)
- `runtime/tests/test_svc_applet_web.c` (real-layout and manual tests)
- `tools/soak/soak.py`
- Docs: `docs/archive/changes/2026-11-01-claude-sched-applets-2026-11-01-r1.md` (new) + `docs/archive/changes/img/2026-11-01-sched-applets-smo-manual.jpg`
  (new); `docs/architecture/runtime-services.md` (two appended paragraphs); `docs/todo.md` (ACNH start flake section only).
- No shared build files touched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: `make -C runtime test` | verified | `make -C runtime GFX=napple test` rc 0 (ipc 33, fs_journal 17, hid_service 10, media/decoder/timescale) |
| `make -C runtime test-host` | verified | rc 0; `test_threads_preempt` 12/12, `test_kernel_sched` 24/24 |
| Applet service tests | verified | `test_svc_applet` 86, `_ae` 52, `_album` 63, `_host` 539, `_libs` 263, `_web` 179 checks, 0 failed |
| P5 / P6 discriminate | verified | P5 fails with `NAPPLE_SLICE_ALL=1`; P6 fails when `core_switch` is forced to queue at the back (patched copy) |
| Acceptance 2: ACNH base 40 x 30 s, 0 null reads, 0 overlaps | **not met** | final runtime: 40 starts, 2 guest crashes (`main+0x1b61d3c` null+0x28), 11 runs with overlap dumps. Base-commit scheduler, same conditions: 20 starts, 0 crashes, 8 runs with overlaps. `NAPPLE_SLICE_MS=0`: 10 starts, 1 crash (null pc `main+0xabb7c` at 6 s, different), 2 with overlaps |
| Acceptance 2: ACNH update+DLC 10 starts | verified | 10/10 ok, 0 overlap dumps (final runtime; also 10/10 with the first fix) |
| Acceptance 2: LAS / SMO / MK8D 60 s | verified | `soak.py run <las, smo, mk8d-base> --runs 1 --seconds 60` with the final runtime: ok / ok / ok (3532 / 3526 / 3290 frames, 0 overlap dumps) |
| Acceptance 3: swkbd on a real title | verified | LAS fresh save, script `20:PLUS,26/30:L+R,34..58 A, 99:L+R, 103..115 A`: `CreateLibraryApplet id 0x11 mode 0` / `swkbd: header "" sub "" guide "" initial "Link" length 1..8 ...` / `swkbd: entered "Napple"`; the next call carries `initial "Napple"`. Config words at the header/sub/guide offsets are all zero in LAS (NAPPLE_VERBOSE dump) |
| Acceptance 4: web applet on a real title | verified | SMO windowed, `NAPPLE_OPEN_MANUAL=25 NAPPLE_SHEET_AUTO=confirm:4000 NAPPLE_SHEET_SHOT=<dir>`: `manual: opening htmlcontents.htdocs/html/USen/index.html ...` / `manual: shown, exit reason 0, last page file:///.../html-document/htmlcontents.htdocs/html/USen/index.html`; screenshot in the change note |
| swkbd inline mode on a title | tested only | no title here uses it (ACNH: own keyboard) |

## Unbuilt / untested list (the integrator checks these first)
None unbuilt. Everything compiles with zero warnings on this Mac.
- `NAPPLE_THREAD_STATS`, `NAPPLE_PREEMPT_SIGNAL` (af25c01, f104cf4): debug aids, run on ACNH (stats) / soaked (knob), off by default.
- The soaks used frozen binaries: `rt-final2` = 2ad0aac code; HEAD only adds the off-by-default `NAPPLE_PREEMPT_SIGNAL` knob (soaked as `rt-knob`) and
  the ACNH update / boots used `rt-final3` = HEAD code.

## Findings
- The ACNH overlap is not a scheduling problem (table above). The b8 "0 of 60 with SLICE_MS=0" was a small 5 s sample.
- Trace facts (main+ offsets, base build): collisions are always 0x51 (core 0) vs 0x57/0x58 (cores 1/2); both in `dispatcher 0x19b7460 -> item method
  0x19b654c -> 0x1af9960 -> 0x1b319e4 -> view vtbl+0x298 (G 0x1ad9018) -> F 0x1ae75c0`. Job pop and item claim are proper `ldxr/stxr` spinlocks. The job
  manager's wait for workers (`0x19b6ed0`) is compiled to discarded loads, so it never waits. The same shape is in the update at `0x390d2b0`, which never
  overlaps, so the real join is elsewhere (not found).
- The two scheduler divergences fixed here are independent of the race and correct per mesosphere. Measured: ACNH thread 0x54 lost core 0 to
  equal-priority rotation 21 times in 25 s before, 0 after.
- LAS: A does not skip the attract movie; its title needs L+R (about 97 s headless from cold). The soak target `las` script (A only) never leaves the
  movie (not changed: other sessions use it as a boot check).
- ACNH's name entry (about 55 s on a fresh save with A every 2 s) is an in-game keyboard; A presses type "1".
- The gfx CBTRACE ring reports `core -9` for every event: `dlsym(RTLD_DEFAULT, "threads_current_core")` fails in the stripped runtime (gfx's
  area, not fixed). The thread id does resolve.
- Decision (user away): the scheduler fixes are kept even though they don't fix ACNH. They are Horizon semantics, unit-tested, and showed no regression
  on LAS/SMO/MK8D/ACNH update. If the integrator prefers to land only proven fixes, revert 2a1aca9 + 2ad0aac (the applet and soak commits are independent).

## How to integrate
- No merge-order constraints. Conflict hotspots: `docs/architecture/runtime-services.md` (two appended paragraphs: keep both sides), `docs/todo.md`
  (only the ACNH section), `tools/soak/soak.py` (if another branch edited it: keep both, the changes are additive).
- Build: `make -C frontend && make -C input && make -C audio && make -C gfx && make -C gfx adapter && make -C runtime GFX=napple`.
- No Linux-only calls added. `applet_web_core.c` stays portable C (uses `opendir`/`access`, POSIX).

## How to test
1. `make -C runtime GFX=napple test`, `make -C runtime test-host` (look for `threads_preempt ... "checked":12,"failed":0`), `make -C runtime GFX=napple build/tests/test_svc_applet_web && runtime/build/tests/test_svc_applet_web` (179 checks).
2. swkbd: LAS fresh save, 118 s headless, `NAPPLE_SWKBD_ANSWER=Napple NAPPLE_INPUT_SCRIPT="20:PLUS:0.3,26:L+R:0.8,30:L+R:0.8,34:A:0.2,...,58:A:0.2,99:L+R:0.8,103:A:0.2,106:A:0.2,109:A:0.2,112:A:0.2,115:A:0.2"`: expect `swkbd: entered "Napple"` at about 109 s and a second call with `initial "Napple"`. Timing depends on load (title at about 97 s).
3. Manual: SMO windowed, `NAPPLE_OPEN_MANUAL=25`: expect `manual: shown, exit reason ...` and the web sheet with the Action Guide. Headless: no sheet, `exit reason 1` (requested).
4. Regression: `NAPPLE_GOLDEN_DATA=<games> python3 tools/soak/soak.py run las|smo|mk8d-base --runs 1 --seconds 60 --lockf <run lock>`: ok. ACNH update 10 x 30 s: ok, `cb=0`.
5. ACNH base race (not fixed): `NAPPLE_GFX_CBTRACE=1 ... soak.py run acnh-base --runs 20 --seconds 30`: expect overlaps in roughly a third of the runs and occasional `main+0x1b61d3c` crashes, the same as before this branch.

## Fix policy hints
- Typos and includes: fix directly. If a title regresses with the scheduler changes (a hang where an equal-priority thread starves another), the
  quick check is `NAPPLE_SLICE_ALL=1`. That would point at a Napple-specific starvation, which needs a design decision, not a revert of the Horizon rule.

## Needs from other areas
- gfx: CBTRACE's `threads_current_core` lookup returns -9 (use `dlsym(RTLD_DEFAULT, ...)` on an exported symbol, or pass the core through the
  runtime's gfx glue). A deeper CBTRACE backtrace (6 host frames cut off the worker's job chain) would help the next investigator.
- Next ACNH step (runtime or gfx): find what the guest main thread waits on before it re-records the views of a job group (see Findings). If it waits
  on GPU progress (an NVN sync), that is gfx.
