# Command-line reference

This page covers the `napple` build-side tool (`crates/napple-cli`), the runtime binary `napple-runtime` (`runtime/src/main.c`) and the scripts in `tools/` that are run directly.

The `napple` binary does not use a command-line parsing library. Each subcommand parses its own arguments by hand, so there is no generated `--help` text. The usage strings quoted
below are the ones the code prints. The dependencies could not be fetched offline when this page was written, so the binary was not built and no help output was captured; everything here
is taken from the source files named in each section.

Conventions:

- `<files...>` are game containers: base game, updates and DLC as XCI, NSP, NSZ or XCZ, in any order. `napple build` also accepts folders (see [build](#napple-build)).
- `-k <prod.keys>` is the user's key file. Keys are read from it and are never printed.
- `--updates` takes `all`, `latest`, `none` or a comma-separated list of version numbers (decimal, for example `65536,131072`). `--dlc` takes `all`, `none` or a comma-separated list of DLC title ids in hex.
- Exit status: `0` on success, `1` for an error (`error: ...` on stderr), `2` for a usage error of the top-level commands `ingest`, `plan` and `decomp`.

## napple (top level)

```
usage:
  napple ingest <game> -k <prod.keys>
  napple plan   <files...> -k <prod.keys> [--updates all|latest|none|<v,v>] [--dlc all|none|<id,id>] [--json]
  napple clean  [--shader-cache] [--build-cache <dir>] [--engine <dir>] [--dry-run]   (removes rebuildable artifacts)
  napple decomp <files...> -k <prod.keys> [--updates ...] [--dlc ...] [-o <dir>] [--json]
files: base game, updates and DLC (XCI/NSP), any order, one or several files.
```

Subcommands implemented in other crates take the rest of the arguments unchanged: `napple rewrite` (`crates/napple-rewrite/src/cli.rs`), `napple compat` (`crates/napple-compat/src/lib.rs`),
`napple sigs` (`crates/napple-sigs/src/cli.rs`) and `napple build` (`crates/napple-build/src/cli.rs`, `crates/napple-build/src/lib.rs`).

Running `napple` with no arguments, with an unknown first word, or without `-k` and at least one file for `ingest`/`plan`/`decomp` prints the usage text to stderr and exits with `2`. `napple --help` does the same.

Environment variables read by these commands are in [environment-variables.md](environment-variables.md#build-cli-packaging-and-studio).

## napple ingest

Synopsis: `napple ingest <game> -k <prod.keys>`

Opens one container and prints what it holds. Only the first file argument is used.

| Option | Default | Meaning |
|---|---|---|
| `<game>` | required | A game container (XCI, NSP, NSZ, XCZ). |
| `-k <prod.keys>` | required | Key file. |

Output: `title id: <16 hex>`, one `exefs <name> (<n> bytes)` line per ExeFS entry, and `romfs: <n> files` when a RomFS is present.

Example:

```
napple ingest ~/Games/game.nsp -k ~/.switch/prod.keys
```

## napple plan

Synopsis: `napple plan <files...> -k <prod.keys> [--updates ...] [--dlc ...] [--json]`

Scans the files and shows the base game, the updates and the DLC that a build would use, with the selection marked.

| Option | Default | Meaning |
|---|---|---|
| `<files...>` | at least one | Base game, updates, DLC. |
| `-k <prod.keys>` | required | Key file. |
| `--updates <sel>` | `latest` | Which updates are marked selected: `all`, `latest`, `none`, or a list of version numbers. |
| `--dlc <sel>` | `all` | Which DLC are marked selected: `all`, `none`, or a list of title ids in hex. |
| `--json` | off | Print one JSON object `{"base","updates","dlc","warnings"}` (format in [studio-protocol.md](../architecture/studio-protocol.md)); no game bytes. |

Example:

```
napple plan base.xci update.nsp dlc.nsp -k prod.keys --updates all --json
```

## napple decomp

Synopsis: `napple decomp <files...> -k <prod.keys> [--updates ...] [--dlc ...] [-o <dir>] [--json]`

Prints the plan, then analyses the selected base and updates: for each module its build id, text size and counts of the sites the rewriter would handle, and the RomFS file count.
A base game is required.

| Option | Default | Meaning |
|---|---|---|
| `<files...>` | at least one | Base game, updates, DLC. |
| `-k <prod.keys>` | required | Key file. |
| `--updates <sel>` | `latest` | Updates analysed. |
| `--dlc <sel>` | `all` | DLC shown as selected. |
| `-o <dir>` | none | Also write `decomp-report.json` into `<dir>` (created if needed). |
| `--json` | off | Print `{"plan":...,"reports":[...]}` instead of the text report. |

Example:

```
napple decomp game.nsp -k prod.keys -o work/decomp
```

## napple rewrite

Usage text:

```
usage: napple rewrite <base/update files or .nso...> -k <prod.keys> -o <outdir> [--updates all|latest|none|<v,v>]
       napple rewrite --survey|--survey-exact <base/update files...> -k <prod.keys> [--updates ...]   (no output; NPDM, modules, sites, relocations, patch size)
```

Runs the AArch64 rewriter on the modules of a game (or on raw `.nso` files, module name = file stem) and verifies the result structurally. Output files and their formats are in
[rewrite-format.md](../architecture/rewrite-format.md). AArch32 modules are skipped with a message. The command fails (exit 1) when any site is unhandled or a check fails.

| Option | Default | Meaning |
|---|---|---|
| `<files...>` | required | Game containers, or raw NSO files (detected by the `NSO0` magic). |
| `-k <prod.keys>` | required for containers | Key file. Not needed for raw NSO files. |
| `-o <outdir>` | required (not for `--survey`) | Output directory. The base goes to `<outdir>`, each selected update to `<outdir>/update-v<version>/`. |
| `--updates <sel>` | `none` | Updates to rewrite as well. |
| `--survey` | off | Print facts per program (NPDM, modules, sites, relocations, patch size) without writing output; exits 1 with a message when the title cannot be built as is. |
| `--survey-exact` | off | Same as `--survey` with the exact site scan. |
| `-h`, `--help` | | Print the usage text (as an error, exit 1). |

Example:

```
napple rewrite game.nsp -k prod.keys -o work/rewritten --updates latest
```

### napple rewrite a32

Usage text: `usage: napple rewrite a32 <modules dir> [-o <work dir>]   (the dir holding main.patched.bin / main.tables ...)`

Lifts the AArch32 modules in a data directory to AArch64 and installs `a32jit.dylib` and `a32jit.manifest` there (see [aarch32.md](../architecture/aarch32.md)). Needs a C compiler.

| Option | Default | Meaning |
|---|---|---|
| `<modules dir>` | required | Directory with the `*.patched.bin` and `*.tables` files of the AArch32 modules. |
| `-o <work dir>` | `<temp dir>/napple-a32jit` | Scratch directory for the generated sources. |

Example:

```
napple rewrite a32 work/games/0100152000022000/<buildid16>/modules
```

## napple build

Usage text (the first lines; the full text is printed on a usage error or `-h`):

```
usage:
  napple build <game files or folders...> -k <prod.keys> -o <outdir> [options]     build <outdir>/<Name>.app
  napple build --list-signing [--json]
  napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]
  napple build --dump-romfs <game> -k <keys> -o <file> | --dump-exefs <game> -k <keys> -o <dir> | --dump-fonts <firmware> -k <keys> -o <dir>
Stdout: PROGRESS <stage> <percent> <message> lines, then RESULT <path to the .app>. Errors: `error: [code] message` on stderr.
```

Builds a self-contained app (or runtime data directories) from the game files. This is the command Studio runs ([studio-protocol.md](../architecture/studio-protocol.md));
the layout of the result is in [module-format.md](../architecture/module-format.md). A folder argument stands for every `.nsp`, `.xci`, `.nsz` and `.xcz` file inside it, recursively.

| Option | Default | Meaning |
|---|---|---|
| `<files or folders...>` | at least one | Game containers. |
| `-k <prod.keys>` | required | Key file. |
| `-o <outdir>` | required | Output directory. The app is `<outdir>/<Name>.app`; with `--data-only` the data directories go here. |
| `--firmware <zip\|dir>` | none | Firmware zip or directory of NCAs, used for the shared system fonts. Without it the build works and records a compatibility note. |
| `--title-keys <title.keys>` | none | Accepted for Studio compatibility; tickets inside NSPs are used. |
| `--updates <sel>` | `latest` | Updates to embed. |
| `--dlc <sel>` | `all` | DLC to embed. |
| `--target <t>` | `macos` | `macos`, `ios` (iPhone and iPad), `iphone`, `ipad` or `ios-sim`. `ios` needs signing options. `--sim` is a shorthand for `--target ios-sim`. |
| `--signing-identity <name\|sha1>` | none | iOS: certificate for `codesign` (see `--list-signing`). |
| `--provisioning-profile <file>` | none | iOS: `.mobileprovision` covering the bundle id and the device. |
| `--bundle-id <id>` | `com.napple.game.<titleid>` | iOS: bundle identifier, a reverse-DNS name. |
| `--ipa` | off | iOS: also write `<Name>.ipa`. |
| `--install <device>` | none | iOS: install on a device with `xcrun devicectl` (identifier, UDID or name). |
| `--launch` | off | With `--install`, start the app after installing. |
| `--name <app name>` | the name in the game's NACP | App name. |
| `--runtime <path>` | next to `napple`, or `runtime/build/napple-runtime` in a checkout | Runtime binary to embed. |
| `--launcher <path>` | `packaging/build/napple-launcher` | Launcher binary to embed. |
| `--make-runtime` | off | Run `make -C runtime` and `make -C packaging` first (checkout only). |
| `--update-romfs <mode>` | `materialize` | `materialize` writes one patched `romfs.bin` per update; `delta` writes a BKTR delta layer; `in-place` (with `--data-only`, for development) writes a `romfs.map` that reads the RomFS straight from the game files. |
| `--keep-cache` | off | Keep `<outdir>/.napple-cache` (resumable RomFS copies). |
| `--no-sign` | off | Skip `codesign` (macOS: no signature; iOS: unsigned app or IPA for an external signer). |
| `--data-only` | off | Write `<outdir>/<titleid>/<buildid16>/` data directories only, no `.app` (runtime development). |

Option checks: the `--signing-*`, `--bundle-id`, `--ipa` and `--install` options need an `ios`, `iphone` or `ipad` target; `ios-sim` is ad-hoc signed and accepts none of the signing options.
Unknown options are an error. Environment: `NAPPLE_TMPDIR`, `NAPPLE_A32_RECOMPILE` (see [environment-variables.md](environment-variables.md)).

Example:

```
napple build base.xci update.nsp -k prod.keys --firmware firmware.zip -o ~/Apps
```

### napple build --list-signing

Synopsis: `napple build --list-signing [--json]`. Lists the iOS signing identities and provisioning profiles on this Mac. `--json` prints machine-readable output.

### napple build --check

Synopsis: `napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]`. Validates the key file (header key, key generations) and optionally a firmware, printing key names and generations only,
never values. Exits 1 when problems are found. Any other argument is an error.

Example: `napple build --check -k prod.keys --firmware firmware.zip --json`

### napple build --dump-exefs, --dump-romfs, --dump-fonts

| Form | Meaning |
|---|---|
| `napple build --dump-exefs <game> -k <keys> -o <dir>` | Write every ExeFS entry (NSOs, `main.npdm`) to `<dir>` and print `title id: <hex>`. |
| `napple build --dump-romfs <game> -k <keys> -o <file>` | Stream the decrypted RomFS image to `<file>` (several GB). |
| `napple build --dump-fonts <firmware zip\|dir> -k <keys> -o <dir>` | Write the shared system fonts and `fonts.json` to `<dir>`. |

Example: `napple build --dump-exefs game.nsp -k prod.keys -o work/exefs`

## napple compat

Usage text:

```
usage:
  napple compat run <folder> -k <prod.keys> [-o work/compat] [--matrix compat/matrix.json] [--md COMPATIBILITY.md]
  napple compat show [titleid]
  napple compat set <titleid> <stage> <status> "<reason>" [--build <id16>] [--evidence <path>]
  napple compat render [--matrix compat/matrix.json] [--md COMPATIBILITY.md] [-o work/compat]
  napple compat npdm <game.nsp|xci> -k <prod.keys>
stages: ingest build-plan nso-load site-scan rewrite rewrite-verify runtime-boot video audio input saves title-screen ingame
status: untested pass partial fail blocked
```

Maintains the compatibility matrix (format: [compat-format.md](../architecture/compat-format.md)). Defaults used by all subcommands: matrix `compat/matrix.json`, Markdown `COMPATIBILITY.md`, stage files `work/compat`
(all relative to the current directory).

| Subcommand | Meaning |
|---|---|
| `run <folder> -k <keys>` | Run the build-side stages (ingest, build-plan, nso-load, site-scan) on every game in the folder, merge the results into the matrix and render the Markdown. |
| `show [titleid]` | Print the matrix, optionally for one title id (16 hex digits). |
| `set <titleid> <stage> <status> "<reason>"` | Record a manual result. `--build <id16>` limits it to one build; `--evidence <path>` stores a path as evidence. Needs the game to be in the matrix already. |
| `render` | Merge the stage files under `-o` into the matrix and write the Markdown. |
| `npdm <game> -k <keys>` | Print the NPDM facts of the application and patch packs (process name, 64-bit flag, stack size, services used and hosted). |

| Option | Default | Meaning |
|---|---|---|
| `-k <prod.keys>` | required for `run`, `npdm` | Key file. |
| `-o <dir>` | `work/compat` | Directory of stage files. |
| `--matrix <file>` | `compat/matrix.json` | Matrix JSON. |
| `--md <file>` | `COMPATIBILITY.md` | Rendered Markdown. |
| `--build <id16>` | all builds | `set`: build id (first 16 hex of the main module id). |
| `--evidence <path>` | empty | `set`: evidence path. |

Example: `napple compat set 0100000000010000 audio pass "music plays" --evidence work/logs/boot.log`

## napple clean

`napple clean [--shader-cache] [--build-cache <game dir>] [--engine <game dir>] [--save-root <dir>] [--dry-run]` (`crates/napple-cli/src/clean.rs`) lists and removes rebuildable artifacts with a size report; it never touches saves, keys, firmware or game files.

| Option | Removes |
|---|---|
| `--shader-cache` (default when no category is given) | `<save-root>/<title id>/cache`: translated shaders and the Metal pipelines archive (the next boot rebuilds them). `--save-root` overrides the default save root. |
| `--build-cache <game dir>` | `<game dir>/.napple-cache.nosync` (resumable RomFS copies left by `napple build --keep-cache`). |
| `--engine <game dir>` | The `napple-runtime`, `napple-runtime-ios` and `napple-launcher` copies inside a built game folder (`napple build` puts them back). |
| `--dry-run` | Lists what would be removed and the sizes; removes nothing. |

## napple sigs

Usage text:

```
usage:
  napple sigs lookup <titleId> <buildId>
  napple sigs match   <module.nso|patched.bin> [--title ID --build ID --name main]
  napple sigs match   --game <xci|nsp> -k <prod.keys> [--module main]
  napple sigs resolve <module> --title ID [--build ID] [--no-rea] OFFSET[:KIND[:NAME[:SIZE]]]...
  napple sigs resolve --game <file> -k <prod.keys> --module main [--no-rea] OFFSET[:KIND[:NAME[:SIZE]]]...
  napple sigs pending
  napple sigs share [--dry-run | --yes]
  napple sigs validate <file.json>
  napple sigs sync                      (explicit network: refresh work/sigcache from the public repo)
common: --work DIR (default ./work)  --known DIR (default ./knownSignatures)  --remote URL
Default is local only: nothing is sent anywhere without `share --yes`; `sync` is the only other network use.
```

Looks up, matches, resolves and shares per-build signatures (data in `knownSignatures/`, see [knownSignatures/README.md](../../knownSignatures/README.md)).

| Subcommand | Meaning |
|---|---|
| `lookup <titleId> <buildId>` | List the signatures known for that build in the layered view (pending, shared, known, cache). |
| `match` | Match the known signatures of a build against a module and report hits, ambiguous hits and misses. A raw module needs `--title` and `--build`. |
| `resolve` | Resolve one or more items (`OFFSET[:KIND[:NAME[:SIZE]]]`, offset decimal or `0x` hex, kind default `function`) using known signatures, signatures carried from other builds, and the REA (Ghidra) adapter. Results are stored locally as pending. Exits 1 if any item is unresolved. |
| `pending` | List the locally stored signatures not yet shared. |
| `share` | Build the contribution, print a byte-for-byte preview and stop. `--dry-run` also checks that `gh` and `git` exist; `--yes` opens the pull request through a fork. |
| `validate <file.json>` | Check a signature file against the rules in `knownSignatures/README.md`; rejects, never trims. |
| `sync` | Refresh the read-only cache `work/sigcache` from the remote (the only network use besides `share --yes`). |

| Option | Default | Meaning |
|---|---|---|
| `--work DIR` | `work` | Work directory (`sigs/pending`, `sigs/shared`, `sigcache`, `rea-in`). |
| `--known DIR` | `knownSignatures` | Directory of committed signatures. |
| `--remote URL` | `https://github.com/erostalevi/Napple` | Repository used by `sync` and `share`. |
| `--title ID`, `--build ID`, `--name NAME` | | Identify a raw module (title id, build id, module name). |
| `--game FILE`, `-k KEYS`, `--module NAME` | | Take the module from a game container instead of a file. |
| `--no-rea` | off | `resolve`: do not call the REA adapter. |
| `--dry-run`, `--yes` | off | `share` modes. |

Example: `napple sigs resolve --game game.nsp -k prod.keys --module main 0x24741f8:function:nnMain`

## Runtime binary (napple-runtime)

Source: `runtime/src/main.c`. Built by `make -C runtime` into `runtime/build/napple-runtime`. Apps built by `napple build` start it through `Contents/MacOS/napple-launcher`
(`packaging/launcher.c`), which passes extra arguments through.

Usage text:

```
usage: napple-runtime <data-dir> [options]
  --headless          no window (graphics back end runs without a layer)
  --seconds N         stop after N seconds (boot tests)
  --trace FILE        JSON-lines trace of svc/IPC events (also NAPPLE_TRACE)
  --summary FILE      write the compatibility stage file runtime-boot.json there
  --save-root DIR     user data root (default ~/Library/Application Support/Napple)
  --svc-trace         log every svc
  -v                  verbose log (also NAPPLE_VERBOSE=1)
```

| Argument | Default | Meaning |
|---|---|---|
| `<data-dir>` | required | Runtime data directory containing `manifest.json`, `modules/` and `main.npdm` ([module-format.md](../architecture/module-format.md)). The first argument not starting with `-`. |
| `--headless` | off | No window; input discovery of GameControllers is off and audio uses the null sink unless `NAPPLE_AUDIO_DEVICE=1`. |
| `--seconds N` | 0 (run until the guest exits or the window closes) | Stop after N seconds. |
| `--trace FILE` | none | Write a JSON-lines trace. |
| `--summary FILE` | none | Write `runtime-boot.json`, the stage file for the compatibility matrix. |
| `--save-root DIR` | `~/Library/Application Support/Napple` | Root for saves, `input.json`, settings and `system/fonts`. |
| `--svc-trace` | off | Log every svc. |
| `-v` | off | Verbose log. |

Any other option prints the usage text and exits with `2` (on iOS unknown arguments are ignored, because the system adds its own). With no data directory the usage text is printed and the exit status is `2`.
While running, `SIGUSR1` dumps thread information and `SIGUSR2` requests a capture of the next 4 frames (`runtime/src/app.m`).

Example:

```
runtime/build/napple-runtime work/games/0100000000010000/<buildid16> --headless --seconds 20 --save-root work/testroot
```

The launcher (`packaging/launcher.c`) takes no options of its own: it picks the version directory (`NAPPLE_VERSION` or `Contents/Resources/game/default`), drops Finder's `-psn_*` argument and runs the runtime with the rest.

## Scripts in tools/

Scripts meant to be run directly. Paths are relative to the `Napple/` root; most scripts `cd` to it themselves.

| Script | Usage | Purpose |
|---|---|---|
| `tools/ci_build.sh` | `tools/ci_build.sh [output-dir]` (default `dist`) | Builds every in-repo dependency, the CLI, Studio and the release artifacts. macOS on Apple Silicon only. Environment: `NAPPLE_VERSION`, `NAPPLE_BUILD`, `NAPPLE_SIGN_IDENTITY`, `NAPPLE_SKIP_TESTS`. |
| `tools/prepare_game.sh` | `tools/prepare_game.sh <game.nsp\|xci> <prod.keys> [--romfs]` | Older pipeline that builds `work/games/<titleid>/<buildid16>/`; `napple build --data-only` replaces it. `--romfs` also writes `romfs.bin`. `REWRITER=python` selects the reference Python rewriter. |
| `tools/boot_test.sh` | `tools/boot_test.sh <titleid> [seconds] [extra runtime args...]` | Runs the newest build under `work/games/<titleid>/` headless for N seconds (default 20), logs to `work/logs`, writes `runtime-boot.json` under `work/compat`. |
| `tools/legacy/flaky_boot.sh` | `tools/legacy/flaky_boot.sh <runtime-binary> <data-dir> <runs> [seconds] <outdir>` | Repeats a headless boot and prints one line per run (exit code, first failure line). |
| `tools/long_run_perf.sh` | `tools/long_run_perf.sh <runtime-binary> <data-dir> <seconds> <outdir> [extra runtime args...]` | Headless long run with `NAPPLE_PERF_LOG=1` and `long_monitor.sh`, then a summary of stalls and memory. |
| `tools/long_monitor.sh` | `tools/long_monitor.sh <pid> <logfile> [interval-s]` | Samples a running runtime (RSS, threads, CPU) every N seconds (default 5). |
| `tools/build_a32jit.sh` | `tools/build_a32jit.sh <data dir> [work dir]` | Lifts the AArch32 modules of a data directory and installs `<data dir>/modules/a32jit.dylib`. |
| `tools/build_ios_deps.sh` | `tools/build_ios_deps.sh` (`SDK=iphonesimulator` for the Simulator) | Builds libopus (and optionally libvpx) for iOS into `build-ios-deps/`. Needs Xcode and cmake. |
| `tools/build_ios.sh` | `tools/build_ios.sh` (`SDK=iphonesimulator` for the Simulator) | Builds the iOS runtime into `runtime/build-ios/napple-runtime`. |
| `tools/refresh_app_runtime.sh` | `tools/refresh_app_runtime.sh <App.app>...` | Replaces the runtime and launcher inside built apps with the current builds and re-signs them ad hoc. |
| `tools/check_docs.py` | `tools/check_docs.py [--root <Napple dir>]` | Checks that relative links and anchors in the Markdown files resolve; exit 0 when everything is fine. |
| `tools/gen_a32_nvn.py` | `python3 tools/gen_a32_nvn.py > runtime/src/a32/a32_nvn_sigs.inc` | Regenerates the AArch32 NVN signature table from `gfx/src/nvnapi/nvn_protos.h`. |
| `tools/addr2sym.py` | `tools/addr2sym.py <data-dir> module+0xoff ...` | Nearest exported symbol for module offsets. |
| `tools/romfs_lookup.py` | `tools/romfs_lookup.py <romfs.bin> <log> <from secs> <to secs>` | Which RomFS files the game read in a time window, from a log made with `NAPPLE_FS_TRACE`. |
| `tools/make_empty_romfs.py` | `tools/make_empty_romfs.py <out>` | Writes a valid empty RomFS image for tests. |
| `tools/linux-check/check-syntax.sh` | `tools/linux-check/check-syntax.sh [file.c ...]` | Syntax-checks the portable service sources with gcc and clang (default: all of `runtime/src/services/*.c`). |
| `tools/linux-check/run-tests.sh` | `tools/linux-check/run-tests.sh [test.c ...]` | Builds and runs `runtime/tests/test_svc_*.c` on Linux against a mock IPC layer. |
| `studio/build_app.sh` | `studio/build_app.sh [output-dir]` | Builds `NappleStudio.app` (ad-hoc signed), building the CLI, runtime and launcher first when missing. |
| `studio/packaging/make_release.sh` | `studio/packaging/make_release.sh <dir containing NappleStudio.app>` | Creates the DMG, the zip, and `SHA256SUMS.txt`. |

The `tools/dev/dev_*.sh` scripts are developer test drivers that take a GPU lock (`work/gpu.lock`); their usage is in [tools/dev/README.md](../../tools/dev/README.md).
`tools/dev/synth_keys.m` and `tools/prepare_tables.py` are specific to one developer machine or are superseded and are not documented here; [tools/README.md](../../tools/README.md) describes every script.
