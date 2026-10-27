# Catalogue field reference

Where each fact about a title is written, which tool reads it, and what the generated tables take from it. The step-by-step workflow is the guide [document a title](../guides/contributing-document-a-title.md).
Metadata only: ids, names, versions, counts, service names, log lines. Never game bytes, keys or firmware.

## The four places
| File | Written by | Holds | Read by |
|---|---|---|---|
| `compat/matrix.json` | `napple compat run` / `set` / `render` (never by hand) | per game and version: name, publisher, `build_id`, `display_version`, and one record per stage (`status`, `reason`, `evidence`, `time`, `tool`, `counters`) | `COMPATIBILITY.md` (rendered), `tools/support/gen_support.py` |
| `COMPATIBILITY.md` | `napple compat render` | the full matrix as Markdown | people; do not edit |
| `docs/titles/<TITLEID>.md` | you | the prose page: state, needs, open items, build and test commands, numbers | `gen_support.py`, the site, people |
| `docs/titles/README.md` | you | one table row per title, including the iOS Simulator column | `gen_support.py` (iOS column), `check_docs.py` (reachability) |
| `docs/symptoms.md` | you | one row per root-caused symptom (Symptom, Cause, Fix, Found on) | `check_docs.py` (4 non-empty cells) |

## Stage results (`compat/matrix.json`)
Stages: `ingest build-plan nso-load site-scan rewrite rewrite-verify runtime-boot video audio input saves title-screen ingame`. Statuses: `untested pass partial fail blocked`. Format and counters: [compat-format.md](../architecture/compat-format.md).

| Status | Use it when |
|---|---|
| `pass` | you ran it and it works as intended |
| `partial` | works with gaps; say which in `reason` |
| `fail` | attempted, does not work |
| `blocked` | cannot be attempted yet; say what blocks it |
| `untested` | nothing known. A headless boot says nothing about `audio` or `input`: leave them `untested` or `partial` |

Record a judgement: `napple compat set <titleid> <stage> <status> "<one sentence reason>" --evidence work/logs/<file>.log` (the game must already be in the matrix; `napple compat run <folder> -k <prod.keys>` adds it).
The runtime fills `runtime-boot` itself with `--summary` ([cli.md](cli.md)).

## Title page fields that feed the generated tables
`gen_support.py` parses `docs/titles/<TITLEID>.md` strictly; a page that breaks these rules still renders but its table row degrades.

| In the page | Becomes | Rule |
|---|---|---|
| First line `# <Name> (<TITLEID>)` | Name column | id is 16 upper-case hex digits and equals the file name |
| `**State:** ...` line | "State per title" list in `support-matrix.md` | one line, what was measured and how; no "played" unless a person did |
| `## What is open` bullets | "Known issues" column (first sentence of each, first 3 shown) | one bullet per issue; start with a full sentence |
| `## Numbers` first bullet | "Performance" column | the headline number first |
| Row in `docs/titles/README.md` (column 4) | iOS column | keep the column count |
| Matrix stages `partial`/`fail`/`blocked` with a `reason` | "Known issues" column, prefixed by stage | write `reason` as one sentence |

Other sections follow the four existing pages: `## What it needs`, `## Build and test`.

## Regenerating the support tables
```bash
python3 -I tools/support/gen_support.py          # rewrites docs/reference/support-matrix.md, feature-support.md, support-data.json
python3 -I tools/support/gen_support.py --check  # exit 1 and the names of stale files; check_docs.py runs it
python3 tools/check_docs.py                       # links, anchors, orphans, symptoms rows, stale tables
```
Commit the three generated files together with your edit. Do not edit them by hand and do not run `--refresh-snapshot` (that is a maintainer step). Tool tests: `python3 -I tools/support/test_gen_support.py`.
