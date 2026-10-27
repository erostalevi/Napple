# Report a bug (capture the log and the machine)

## Goal
Give a maintainer everything needed to reproduce a problem without asking you back: what you ran, on which build and Mac, and the log. Use this for the *Bug report* form; for a game that misbehaves use the
[compatibility report](contributing-document-a-title.md), for wrong pixels the [graphics bug](contributing-report-a-graphics-bug.md) guide. Before you start, search [symptoms.md](../symptoms.md): the cause may be known.

## Never include
Keys, firmware, game files, dumps, extracted assets ([CONTRIBUTING.md](../../CONTRIBUTING.md#legal-rules-first)). If a log line contains a path with your user name or a long hex string you do not recognise, remove it.

## Steps
1. **Machine.** `sysctl -n machdep.cpu.brand_string; sw_vers -productVersion`.
2. **Napple build.** Release name from Studio's About, or from source `git rev-parse --short HEAD`.
3. **Re-run with logging.** The runtime writes everything to stderr:
   ```bash
   mkdir -p /tmp/napple-save
   runtime/build/napple-runtime <data-dir> --headless --seconds 45 --save-root /tmp/napple-save --summary /tmp/napple-summary.json 2> /tmp/napple-boot.log; echo "exit $?"
   ```
   Exit `0` is normal, `70` a `FAIL:` line, `128+n` a `CRASH`. Use a fresh save root so old saves do not matter. For the build step capture the CLI output (`napple build ... 2>&1 | tee build.log`).
4. **Trim the log.** Paste the last 100 lines and every line containing `CRASH`, `FAIL:`, `warn:` or `error:`. Attach the whole file only if it is small and clean. `~/Library/Logs/Napple/<titleid>.log` has the capped log of an app started from Finder.
5. **More detail when asked.** `-v` logs every IPC command, `--svc-trace` every svc, `--trace FILE` writes JSON lines ([debugging.md](debugging.md), [environment variables](../reference/environment-variables.md)). A crash prints `CRASH signal=... pc=<module>+0x<offset>`: copy that block whole.
6. **Reproducibility.** Say whether it happens every run, and whether a second fresh run reproduces it.

## What a good report has
One sentence title, exact steps, expected versus actual, build and machine, the trimmed log, and the answer to "did you check docs/symptoms.md". Open it with the *Bug report* form.
