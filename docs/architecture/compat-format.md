# Compatibility stage-result format (for tools and the runtime)

The compatibility matrix (`compat/matrix.json`, rendered to `COMPATIBILITY.md`) is built from small JSON files,
one per (game, build, stage). Any tool, including the C/ObjC runtime, reports a result by writing one file.
`napple compat render` (or `napple compat run`) merges them into the matrix. Files never contain game bytes.

## Location

```
work/compat/<TITLEID>/<BUILDID>/<stage>.json
```

- `TITLEID`: the **application (base) title id**, 16 hex digits (`0100000000010000`; for an update/DLC use the base id).
  Case-insensitive on read; write uppercase.
- `BUILDID`: first **16 hex chars (lowercase)** of the main NSO build id, which is the 32-byte ModuleId in the NSO
  header at 0x40, hex-encoded; the first 8 bytes. The runtime already knows it from the NSO header it loads.
  `napple compat show <titleid>` prints it. `unknown` is used only when the build id could not be read.
- `stage`: one of
  `ingest build-plan nso-load site-scan rewrite rewrite-verify runtime-boot video audio input saves title-screen ingame`.
  Owned by napple-compat: ingest, build-plan, nso-load, site-scan. Reserved for the rewriter: rewrite, rewrite-verify.
  For the runtime: runtime-boot (reached the guest `main`), video (frames presented), audio, input, saves,
  title-screen, ingame.
- Write atomically (write `<stage>.json.tmp`, then `rename`). Create the directories as needed. Overwrite on each run;
  the file with the newest `time` wins when it is merged with the matrix.

## File content

```json
{
  "stage": "runtime-boot",
  "status": "partial",
  "reason": "reached nnMain, aborted at first nvn call",
  "evidence": "work/runtime/logs/0100000000010000-20261020-0215.log",
  "time": 1760926000,
  "tool": "runtime",
  "counters": {
    "unsupported_svcs": [38, 51],
    "missing_ipc": ["fsp-srv:OpenSaveDataFileSystem", "nvdrv:Ioctl 0x40044801"],
    "missing_nvn": ["nvnQueueSubmitCommands"],
    "crashes": [{"signal": 11, "pc": "0x00000071a0003c10", "note": "null deref in sdk"}],
    "fps_notes": ["avg 24 fps in the first scene"]
  }
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `status` | string | yes | `untested`, `pass`, `partial`, `fail`, `blocked` (case-insensitive on read; write lowercase) |
| `reason` | string | recommended | one short sentence: why this status |
| `evidence` | string | recommended | path of a log/artifact, relative to the repo root, under `work/` |
| `time` | integer | recommended | Unix seconds (UTC). If omitted, the file mtime is used |
| `tool` | string | no | producer name |
| `stage` | string | no | informational; the file name is authoritative |
| `counters` | object | no | every member optional; arrays are unioned across the stages of a build |

Status meaning: `pass` works as intended; `partial` works with known gaps (say which in `reason`); `fail` was
attempted and does not work; `blocked` cannot be attempted yet (a previous stage or a missing component, say which);
`untested` nothing known.

`counters` members: `unsupported_svcs` (integers, SVC numbers the guest hit that the runtime lacks),
`missing_ipc` (strings `service:Command` or just the service name), `missing_nvn` (NVN function names),
`crashes` (objects with `signal` integer, `pc` guest address as a string or a number, `note` string),
`fps_notes` (free text). Unknown members are ignored, strings are not length-limited but a file over 4 MiB is rejected.
A malformed file is skipped with a warning; it never corrupts the matrix.

## Tiny C-friendly writer (no JSON library needed)

Escape `"` and `\` in strings and write control characters as spaces. Example:

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* mkdir -p for "root/TITLE/BUILD" (3 levels); ignore EEXIST */
static void mk(const char *p) { mkdir(p, 0755); }

static void jstr(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fputc(' ', f);
        else fputc(*s, f);
    }
    fputc('"', f);
}

int compat_report(const char *root, const char *title, const char *build16,
                  const char *stage, const char *status, const char *reason, const char *evidence) {
    char dir[512], tmp[560], path[560];
    snprintf(dir, sizeof dir, "%s/%s", root, title);            mk(root); mk(dir);
    snprintf(dir, sizeof dir, "%s/%s/%s", root, title, build16); mk(dir);
    snprintf(path, sizeof path, "%s/%s.json", dir, stage);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "{\"stage\":"); jstr(f, stage);
    fprintf(f, ",\"status\":"); jstr(f, status);
    fprintf(f, ",\"reason\":"); jstr(f, reason);
    fprintf(f, ",\"evidence\":"); jstr(f, evidence);
    fprintf(f, ",\"time\":%lld,\"tool\":\"runtime\"", (long long)time(NULL));
    fprintf(f, ",\"counters\":{\"missing_ipc\":[\"fsp-srv:OpenSaveDataFileSystem\"]}}\n"); /* optional */
    if (fclose(f) != 0) return -1;
    return rename(tmp, path);
}
```

`root` is `work/compat` (the default of `napple compat`). Then run `napple compat render` to refresh
`compat/matrix.json` and `COMPATIBILITY.md`. Manual correction: `napple compat set <titleid> <stage> <status> "<reason>"`.

## matrix.json (committed)

`{"schema":1,"last_run":<unix>,"last_run_iso":"...","games":[{title_id,name,publisher,versions:[{kind,title_id,version,
display_version,build_id,stages:{<stage>:{status,reason,evidence,time,tool,counters?}},modules:[...],npdm:{...},notes:[...]}],dlc:[...],notes:[...]}]}`.
Only metadata: ids, counts, names, service names. Notes starting with `[auto]` are regenerated by `compat run`; other notes are kept.
