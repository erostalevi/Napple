# Debugging and crash triage

## Goal
Take a run that stops, hangs, draws nothing or runs slowly and turn it into a cause, using only the runtime's own output and a few environment variables. The full list of variables is in
[environment-variables.md](../reference/environment-variables.md); this page names a variable only where it matters to the workflow, and each one named here was checked against a `getenv` in the source.
Known causes are indexed by symptom in [symptoms.md](../symptoms.md): look there first, then add a row when you root-cause something new. Per-title state: [titles/](../titles/README.md).
Nothing here runs on Linux except reading logs: the runtime, gfx and everything that runs a guest are macOS-only.

## 1. Make the run reproducible
```bash
mkdir -p /tmp/napple-save
runtime/build/napple-runtime <data-dir> --headless --seconds 45 --save-root /tmp/napple-save \
    --trace /tmp/boot.trace.jsonl --summary /tmp/boot.summary.json 2> /tmp/boot.log; echo "exit $?"
```
- `--headless` has no window (graphics runs on an offscreen target), `--seconds N` stops after N seconds with `time limit of N s reached` and exit 0, `--save-root` is the user data root (fresh for each experiment: saves are transactional and
  persist), `--trace FILE` (or `NAPPLE_TRACE`) writes JSON lines, `--summary FILE` (or `NAPPLE_SUMMARY`) writes the compat stage file, `-v` (or `NAPPLE_VERBOSE=1`) logs every IPC command, `--svc-trace` (or `NAPPLE_SVC_TRACE=1`) logs every svc.
- Everything goes to stderr, one line each as `[napple <seconds>] <tag>: <text>`; gfx lines are `[napple-gfx] <level>: <text>`. When stderr is neither a terminal nor a regular file (an app started from Finder) a filter thread keeps a capped
  log in `~/Library/Logs/Napple/<titleid>.log` (5 MB, three files); `NAPPLE_FILE_LOG=all` keeps every line, `NAPPLE_FILE_LOG=0` disables it (`runtime/src/filelog.c`).
- Exit status: `0` normal, `70` a `FAIL:` line (`runtime_fail`), `128 + signal` a `CRASH`. The same outcome is in the summary file: `status` and `reason`, then `counters` with `unsupported_svcs`, `missing_ipc`, `stubbed_ipc`,
  `implemented_ipc`, `missing_nvn` (only filled by the stub back end), `crashes` and `notes` (`runtime/src/log.c: summary_write`).
- To attach a debugger or sampler: `NAPPLE_WAIT_SECS=<n>` sleeps at start-up and prints the pid.

## 2. Read the end of the log
| You see | Meaning | Next |
|---|---|---|
| `FAIL: IPC service "<name>" not implemented` | the game asked `sm` for a name nobody registered (and the NPDM allows it) | [add-ipc-service.md](add-ipc-service.md) |
| `FAIL: IPC command <service>:<id> not implemented` after an `ipc: ...: command <id> not implemented (type .., N argument bytes, buffers x.. a.. b.. w.. c.., N copy + N move handles)` line | known service, unknown command | same; the line gives the argument size and buffers |
| `FAIL: unimplemented svc 0x.. (<Name>) at <module>+0x..` | svc without a handler | [add-svc.md](add-svc.md) |
| `FAIL: guest Break (reason .., result ..) from <module>+0x..` after `kernel: guest Break ...` and a thread dump | the game aborted itself (`svc Break`); `result` is a Horizon result (module = low 9 bits, description = next 13) | find which check failed, usually a service answer |
| `[napple] CRASH signal=<n> fault=0x<addr> pc=<module>+0x<off>` then `esr=`, `nearest region start/size/prot`, `pc= lr= fp= sp=`, `x0=..x28=`, `frame lr=.. <module>+0x..` | a fault in guest or host code that nobody handled (section 3) | section 3 |
| `[napple] host abort, call stack:` and a backtrace | `abort()` in host code, including `gfx_die` (`[napple-gfx] error: fatal: ...` just before) | a runtime or gfx bug, not the guest |
| Exit 0 early, `kernel: guest called ExitProcess` | the guest quit | read what it logged before (`guest-log`, `guest-debug` tags carry its own messages) |
| No error, but `frames 0`, or frames stop growing | a hang or a spin | section 4 |
| `[napple-gfx] warn: unimplemented NVN entry point called: <name>` | missing NVN function | [add-nvn-function.md](add-nvn-function.md) |
| `[napple-gfx] warn: shader <hash> (stage <n>) could not be translated: <why>` | missing Maxwell op | [add-maxwell-shader-op.md](add-maxwell-shader-op.md) |
| `CallCommands: stale or invalid command handle ... ignored` | recordings released too early (budget) | [symptoms.md](../symptoms.md), Rendering |
At the end `[napple-gfx] stats: frames N draws N ... unimplemented calls N` and `diag:` lines summarise graphics.

## 3. Crashes: `signals.c`
`runtime/src/signals.c` installs one handler for SIGSEGV, SIGBUS, SIGILL and SIGTRAP on an alternate stack (`signals_install`), and a SIGABRT handler that prints the host call stack.
In order, `crash()`:
1. For SIGILL it **emulates** two EL0 instructions macOS traps but Horizon allows: `mrs Xt, CTR_EL0` and `dc civac, Xt`, and resumes.
2. With `NAPPLE_WATCH=<module>+0x<off>` it emulates the faulting store to the watched 8-byte location and logs it (see below); with a host read guard set (`runtime_fault_guard`) it longjmps back.
3. It offers the fault to `hid_trace_fault` (only with `NAPPLE_HID_TRACE`) and to `guest_fault` -> gfx (`napple_gfx_handle_fault`): write faults on pages gfx write-protected for texture tracking are **normal** and are retried, not crashes.
4. Anything else prints the `CRASH` block (module-relative pc via `module_describe`, the 29 registers, up to 24 frame-pointer frames), writes `crashes` and `status: fail` into the summary, drains the file log and exits `128 + signal`.

How to read it:
- `pc=<module>+0x<off>` is the guest module and the offset **in the original image** (`main`, `sdk`, `subsdk0`...). A pc in `runtime` or an unnamed address means host code crashed.
  `python3 tools/addr2sym.py <data-dir> main+0x<off>` prints the nearest exported symbol from `modules/<name>.tables` (NSOs are stripped: only exports have names).
- `fault=0x38`, `0x3e74` and similar small addresses mean a null object plus a field offset: find who should have created the object (the Mii case in [symptoms.md](../symptoms.md) was a null singleton built from a service answer).
- `nearest region ... prot` tells whether the address is unmapped, mapped read-only, or inside a reservation (`esr` bit 6 distinguishes write from read in `guest_fault`).
- The frame chain follows x29; a leaf function crash loses its own frame, use `lr` first.
- `NAPPLE_PEEK=<module>+0x<off>` logs the 8 bytes there at crash time (`runtime_peek`); `NAPPLE_WATCH=<module>+0x<off>` logs every guest write to a location with pc, caller and value.
- AArch32 titles: the crash handler also prints the interpreter's CPU state (`a32_crash_report`); `NAPPLE_A32_TRACE=1` runs one instruction at a time and dumps the last 4096 pcs when the guest calls Break (slow); `NAPPLE_A32_SVCTRACE=1` traces svcs.
- Flaky crashes: run the same boot repeatedly with `tools/legacy/flaky_boot.sh <runtime-binary> <data-dir> <runs> [seconds] <outdir>` (one line per run: exit code and first FAIL/crash line). The Animal Crossing command-buffer race was found this way
  ([symptoms.md](../symptoms.md), "one run in three").

## 4. Hangs and spins
- `kill -USR1 <pid>` logs every guest thread without stopping the game (`thread_dump_all`): handle, id, priority, core, what it waits on (handles with their types, or an address and key), last svc, call count, the guest call stack,
  and for a thread that is running guest code its live pc. Works headless (`runtime/src/app.m: install_usr1_dump`). `tools/dev/dev_usr1test.sh <datadir> <secs> <first-usr1-at> <name> [more gaps]` automates dumps at given times.
- `NAPPLE_PCPROF=1` samples the pc of every running (not waiting) guest thread each millisecond and, every 5 s, logs the hottest pairs as `pcprof: thread 0x.. pc <module>+0x.. : N samples`. A thread that owns nearly all samples in one place
  is a spin: the Animal Crossing Npad reader (`sdk+0x5087xx`) was found this way ([symptoms.md](../symptoms.md)). `NAPPLE_PCPROF_REGS=1` adds register dumps for one thread id hard-coded in `threads.c`.
- A spin on `WaitSynchronization(handle 0) -> 0xe401` (25 million calls a second) shows with `--svc-trace` and means a reply carried the wrong object/handle ([symptoms.md](../symptoms.md), Animal Crossing boot spin).
- `NAPPLE_FREE_CORES=1` disables the three virtual application cores; if a hang disappears, suspect a blocking service that does not release its core ([add-svc.md](add-svc.md)).

## 5. Tracing IPC and services
- `NAPPLE_TRACE_IPC=all` (or `-v`) logs every command as `ipc: <iface>::<Name> (cmd <id>) in=<bytes> bytes x.. a.. b.. w.. c..` (`runtime/src/ipc.c: note_command`). In the code the value is compared with the literal `all`; a service name is **not** accepted
  (the subsystem page's "or a service name" does not match the code).
- The JSON-lines trace (`--trace`) has one object per event with `t` seconds and `k` kind: `ipc` (`service`, `cmd`, `name`, `bytes`, `status` = `impl`, `stub` or `missing`), `service_open`, `service_refused`, `svc_unsupported`, `stage`, `module_init`, `fail`.
  `grep '"status":"missing"'` lists the gaps without reading the log.
- `NAPPLE_IPC_LENIENT=1` / `NAPPLE_SVC_LENIENT=1` turn "stop at the first missing command/svc" into an error result so one run lists several; the guest then often misbehaves, so use them to enumerate, not to judge.
- Targeted traces: `NAPPLE_FS_TRACE=1` (file system calls with host paths; `a` for every RomFS read), `NAPPLE_APPLET_TRACE` (guest stack on selected applet commands), `NAPPLE_HID_TRACE` (HID shared-memory access),
  `NAPPLE_INPUT_LOG=1` (every change of player 1's merged input state), `NAPPLE_HLE_TRACE=1|2` (counts host-override calls; `2` logs the first call of each).

## 6. Graphics
- Screenshots of what was really rendered: `NAPPLE_SCREENSHOT=/tmp/shot` writes `/tmp/shot-<frame>.png` for the frames in `NAPPLE_SCREENSHOT_FRAMES=1,60,300` (default every 120th, at most 20 files) (`runtime/src/gfx_glue.c`).
  `kill -USR2 <pid>` captures the next 4 presented frames into `NAPPLE_CAPTURE_DIR` (default the current directory) as `cap<serial>-<frame>.png` and enables per-draw diagnostics for them.
- Commands: `NAPPLE_NVNTRACE=1` prints each recorded op number (`[nvntrace] cb <ptr> op <n>`), `NAPPLE_GFX_CMDTRACE=1` the lifetime of recordings (handles), `NAPPLE_GFX_LOG=0..3` the gfx log level (0 error, 1 warn, 2 info = default, 3 debug).
  `NAPPLE_DRAWLOG=<frame>` or `<from>-<to>` prints every draw of those frames (`NAPPLE_DRAWLOG_QUIET=1` keeps only pass dumps, `_RT`, `_RT_ALL`, `_TEX`, `_UBO=<words>` add detail); bisect a wrong object by skipping draws with `NAPPLE_DEBUG_SKIP_VS`;
  `NAPPLE_SCALE_DIAG=1` finds passes that mix scaled and unscaled attachments; `NAPPLE_NO_ORPHAN=1` disables the stopgap present pick (see [titles/01006F8002326000.md](../titles/01006F8002326000.md)).
- Shaders: `NAPPLE_SHADER_DUMP=<dir>` writes programs that fail to translate; with `NAPPLE_SHADER_DUMP_HASH=<hash hex>` also a given one; `NAPPLE_SAVE_MSL=1` annotates generated MSL with pcs; `NAPPLE_SHADER_PROBE` shows registers as colour ([add-maxwell-shader-op.md](add-maxwell-shader-op.md)).
- Performance: `NAPPLE_PERF_LOG=1` prints every 5 s
  `[perf] fps <f> maxgap_ms <ms> gpu_ms_max <ms> out_ms <ms> shown_fps <f> frames <n> | pipes+<n> shaders+<n> pipewait_ms+<n> skipped+<n> pending <n> | blockbuild_ms+<n> max <n> | cmdrecs live <n>` (`gfx/src/mtl_present.m`).
  Stutter is a high `maxgap_ms` with `pipes+` or `blockbuild_ms` non-zero; growing memory is `cmdrecs live`. `NAPPLE_GFX_PROF=1` accumulates wait times inside gfx. `tools/long_run_perf.sh <runtime-binary> <data-dir> <seconds> <outdir>`
  runs a long headless run with the perf log plus RSS/thread/CPU sampling (`tools/long_monitor.sh`) and summarises stalled windows.
- Game speed and pacing are the guest clock's: see "Frame pacing and game speed" in [runtime-services.md](../architecture/runtime-services.md); headless runs pace presents at the swap interval.

## 7. Scripted input
`NAPPLE_INPUT_SCRIPT="5.0:A,12:PLUS:0.3,20:LS_UP:1.5"` (`runtime/src/input_script.c`): comma-separated events `<time>:<button>[:<hold>]`.
- `<time>` is **seconds** (a float) after the guest first configures HID, `<hold>` is seconds (default 0.15). (`docs/titles/README.md` calls the first field "frame"; the code reads seconds.)
- Buttons: `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN`, and `STICK_L STICK_R` (the stick clicks); sticks: `LS_LEFT LS_UP LS_RIGHT LS_DOWN` and `RS_*`. `L+R` presses a chord atomically (same sample).
- It injects through the key bindings of `input.json` (the defaults), so the whole input path is exercised. `script: no key bound for event N` means the binding is missing; `script: unknown button X` a typo. The log shows `input script: N events` and each `script: press <button> at <t> s`.
- In zsh, `"..:$t:A"` is a path modifier: write the script literally ([symptoms.md](../symptoms.md)). Real key events instead of the script: `tools/dev/synth_keys.m` (needs the Accessibility permission), used by `tools/dev/dev_wintest2.sh` and `tools/dev/dev_apptest.sh`.

## 8. The four reference titles
Build data without disk pain (in-place RomFS, about 250 MB each) as in [titles/README.md](../titles/README.md#building-test-data-without-disk-pain-all-titles), then:
```bash
runtime/build/napple-runtime <scratch>/<TITLEID>/<buildid16> --headless --seconds 45 --save-root /tmp/napple-save
```
Passing: exit 0, `[napple-gfx] stats: frames N` with N in the thousands, no `CRASH`. Take the newest version directory for an updated title.

| Title | Id | Page | Notes taken from the page |
|---|---|---|---|
| Link's Awakening | `01006BB00C6F0000` | [page](../titles/01006BB00C6F0000.md) | 2290-2723 frames in 45 s headless; needs fonts for text; `NAPPLE_INPUT_SCRIPT` reaches the file select |
| Super Mario Odyssey | `0100000000010000` | [page](../titles/0100000000010000.md) | 1921-2232 frames in 45 s; `NAPPLE_INPUT_SCRIPT="30:A:0.3,45:A:0.3,60:A:0.3"` boots the first kingdom; NVN 53.x |
| Animal Crossing: New Horizons | `01006F8002326000` | [page](../titles/01006F8002326000.md) | 3.0.3 + DLC; about 1150-1200 frames in 45 s; frames 0 -> `NAPPLE_PCPROF=1` |
| Mario Kart 8 Deluxe | `0100152000022000` | [page](../titles/0100152000022000.md) | update + DLC in-place data; `napple-runtime <out>/0100152000022000/fe941ed5ba14be5d --headless --seconds 120`; `NAPPLE_INPUT_SCRIPT="15:A,19:A,...,140:A"` reaches races; 5362 frames in 120 s |

Game files live outside the repository; delete scratch data when done. Only one game process should use the GPU at a time (`tools/dev/README.md`), and `pgrep -f napple-runtime` matches other sessions' runs: include the data directory in the pattern and never `pkill` by name.

## 9. After the cause is known
Fix it in the narrowest place (a service handler, an svc, an NVN function), add a unit test where the code is portable ([testing.md](testing.md)), re-run the title, then add a row to [symptoms.md](../symptoms.md)
(Symptom, Cause, Fix, Found on; `tools/check_docs.py` requires four non-empty cells) and update the title page.
