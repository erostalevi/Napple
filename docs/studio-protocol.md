# Studio <-> CLI protocol

Studio runs `napple` as a subprocess (no shell). Stdout and stderr lines are shown in the log panel.

## `napple plan <files...> -k <prod.keys> --updates all --dlc all --json`
One JSON object on stdout: `{"base":Item|null,"updates":[Item],"dlc":[Item],"warnings":[str]}`,
`Item = {title_id, version(u32), version_label, size(bytes), selected, path}`. `napple decomp ... --json` prints
`{"plan":<above>,"reports":[...]}`. Unknown fields are ignored by Studio.

## `napple build` (invocation Studio uses)
`napple build <files...> -k <prod.keys> [--title-keys <title.keys>] [--firmware <zip|dir>] --updates <none|v,v> --dlc <none|idhex,idhex> --target macos -o <outdir>`
Studio embeds every selected update/DLC; the built app toggles them at runtime. If the command is missing
(usage text, exit 2) Studio shows "not available yet". Implemented by `crates/napple-build` (no Python, all in process).

Other options (all optional): `--name <app name>` (default: NACP name), `--runtime <path>` / `--launcher <path>` (prebuilt binaries; default: next to `napple`,
`../libexec/napple`, `../share/napple`, or `runtime/build/napple-runtime` and `packaging/build/napple-launcher` in a checkout), `--make-runtime`
(runs `make -C runtime` and `make -C packaging` first, checkout only), `--update-romfs materialize|delta`, `--keep-cache`, `--no-sign`, `--data-only`
(runtime data dirs instead of an .app). `--updates` defaults to `latest`, `--dlc` to `all`; `--title-keys` is accepted but not applied yet (tickets in the NSP/XCI are used).
Preflight: `napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]` validates keys/firmware (key names/generations only, never values; exit 1 on problems).

## Stdout lines
- `PROGRESS <stage> <percent 0-100> <message...>` e.g. `PROGRESS rewrite 12.0 v0 (release 0): main (1 gates, 5 relocations)`. The percent is the
  overall build progress (monotonic, one bar). Stages in order: `preflight ingest plan rewrite romfs dlc fonts bundle sign done`
  (`rewrite` 8-24, `romfs`+`dlc` 24-88 weighted by bytes, `fonts` 90, `bundle` 94, `sign` 99, `done` 100).
- `RESULT <absolute path to the built .app>` (optional; otherwise the first .app in `-o`)
- anything else is a log line (`warning: ...`, `note: ...` for compat notes such as missing fonts). Errors: `error: [<code>] <message>` on stderr (plus
  `ERROR <code> <message>` on stdout) and non-zero exit. Codes: `usage keys input container integrity unsupported plan rewrite bktr firmware runtime tables
  disk-space io sign target check cancelled`. Never print key values.
- Cancel: SIGTERM/SIGINT stop the build between chunks (<= 8 MB), remove the staging directory and exit 1 with `[cancelled]`. A complete RomFS copy
  made before a failure/cancel stays in `.napple-cache.nosync` (in the temp dir when it is on the output volume, else in `-o`) so a retry skips it.
