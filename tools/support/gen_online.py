#!/usr/bin/env python3
"""Online support tables, rendered from compat/online.json (offline, stdlib only). Called by gen_support.py, which writes and --checks them with the
other support tables; running this file on its own does the same for the online outputs only.

Outputs:
  docs/reference/online-support.md   full table, per-title notes and what each mode means
  README.md                          the short table between <!-- online:begin --> and <!-- online:end --> (nothing else in README is touched)
  site/online.json                   the same data as a standalone file (also embedded in site/compat.json by gen_site.py)

Online play in Napple is between Napple players only, through the Napple-run servers (napple-online and napple-room at pretend.napple.lat),
never Nintendo's.

compat/online.json schema (schema 1); edit only this file, then run python3 -I tools/support/gen_support.py:
  {
    "schema": 1,
    "servers": "pretend.napple.lat",                     host of the Napple-run servers, shown in the explanation
    "titles": {
      "<16 hex title id>": {
        "name": "Mario Kart 8 Deluxe",                   same name as docs/titles/<id>.md
        "short": "MK8D",                                 optional short name
        "backend": "nex" | "npln" | "own-servers" | "none",
        "modes": {                                       every mode in MODES below must be present
          "<mode>": {"status": "works" | "partial" | "broken" | "untested" | "unsupported",
                     "note": "one line: what was seen, or why not",
                     "checked": "YYYY-MM-DD"}            the day the status was last checked
        }
      }
    }
  }
  "unsupported" means the game itself has no such mode; "untested" means it has one and nobody has tried it on Napple yet.

usage: python3 -I tools/support/gen_online.py [--root <Napple dir>] [--check]
"""
import argparse
import json
import re
import sys
from pathlib import Path

DATA = "compat/online.json"
DOC = "docs/reference/online-support.md"
README = "README.md"
SITE = "site/online.json"
OUTPUTS = [DOC, README, SITE]
BEGIN, END = "<!-- online:begin -->", "<!-- online:end -->"
ID_RE = re.compile(r"^[0-9A-F]{16}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

# key, column label, short README label, what it means
MODES = [
    ("lan_play", "LAN Play", "LAN", "The game's LAN mode, its broadcast traffic carried between players by a Napple room (napple-room)."),
    ("local_wireless", "Local wireless", "Local", "The game's local wireless play (LDN), emulated over the network through a Napple room."),
    ("online_matchmaking", "Online matchmaking", "Matchmaking",
     "Logging in to napple-online, reaching the game's online menu and being matched with other players into one session (gathering)."),
    ("online_race_or_match", "Online race / match", "Race / match",
     "Actually playing the matched session together: the race, battle or visit, with players talking peer to peer."),
    ("friends", "Friends", "Friends", "Friend list, presence and joining friends, between Napple accounts."),
    ("rankings", "Rankings", "Rankings", "Leaderboards uploaded to and read from the Napple server."),
    ("datastore", "DataStore", "DataStore", "Shared user content stored on the Napple server: ghosts, designs, levels and similar."),
]
MODE_KEYS = [m[0] for m in MODES]
STATUSES = {
    "works": "verified working on Napple",
    "partial": "parts work; the note says what is missing",
    "broken": "tried and fails; the note says how",
    "untested": "the game has this mode, nobody has tried it on Napple yet",
    "unsupported": "the game has no such mode",
}
BACKENDS = {
    "nex": "NEX, the game-server protocol of most first-party games; served by napple-online",
    "npln": "NPLN, the newer game-server protocol",
    "own-servers": "the publisher's own servers, not run by Napple",
    "none": "no online features",
}


def cell(text):
    return re.sub(r"\s+", " ", str(text)).replace("|", "\\|").strip()


def load(root):
    path = root / DATA
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        sys.exit(f"{DATA}: {e}")
    errs = []
    if data.get("schema") != 1:
        errs.append("schema must be 1")
    if not isinstance(data.get("titles"), dict) or not data["titles"]:
        errs.append("titles must be a non-empty object")
    for tid, t in (data.get("titles") or {}).items():
        where = f"titles.{tid}"
        if not ID_RE.match(tid):
            errs.append(f"{where}: title id must be 16 upper-case hex digits")
        if not str(t.get("name", "")).strip():
            errs.append(f"{where}: name missing")
        if t.get("backend") not in BACKENDS:
            errs.append(f"{where}: backend {t.get('backend')!r} not one of {', '.join(BACKENDS)}")
        modes = t.get("modes") or {}
        for k in sorted(set(modes) - set(MODE_KEYS)):
            errs.append(f"{where}.modes.{k}: unknown mode (known: {', '.join(MODE_KEYS)})")
        for k in MODE_KEYS:
            m = modes.get(k)
            if not isinstance(m, dict):
                errs.append(f"{where}.modes.{k}: missing")
                continue
            if m.get("status") not in STATUSES:
                errs.append(f"{where}.modes.{k}: status {m.get('status')!r} not one of {', '.join(STATUSES)}")
            if not str(m.get("note", "")).strip() or "\n" in str(m.get("note", "")):
                errs.append(f"{where}.modes.{k}: note must be one non-empty line")
            if not DATE_RE.match(str(m.get("checked", ""))):
                errs.append(f"{where}.modes.{k}: checked must be YYYY-MM-DD")
    if errs:
        sys.exit(f"{DATA} is invalid:\n  " + "\n  ".join(errs))
    return data


def titles(data):
    return [dict(t, title_id=tid) for tid, t in sorted(data["titles"].items(), key=lambda kv: (kv[1]["name"].lower(), kv[0]))]


def last_checked(data):
    return max(m["checked"] for t in data["titles"].values() for m in t["modes"].values())


def mark(status):
    return "-" if status == "unsupported" else status


def render_doc(data):
    servers = data.get("servers", "the Napple servers")
    ts = titles(data)
    L = ["# Online support", "",
         "Per-game status of online and local multiplayer on Napple. **Generated** by `tools/support/gen_support.py` (module `tools/support/gen_online.py`) "
         "from `compat/online.json`; do not edit by hand (edit the JSON, run `python3 -I tools/support/gen_support.py`; `--check` fails when this page, "
         "the README block or `site/online.json` is stale).", "",
         f"**Online play is between Napple players only**, through the Napple-run servers at `{servers}` (napple-online for accounts and the game "
         "servers, napple-room for LAN Play and local wireless rooms). Napple never connects to Nintendo's servers, and Napple players cannot meet "
         "players on a console. See [network providers](../guides/network-providers.md) and [deploying the servers](../guides/deploy-servers.md).", "",
         f"Last checked: {last_checked(data)}. Statuses: `works`, `partial`, `broken`, `untested`; `-` (unsupported) means the game has no such mode.", "",
         "## By title", "",
         "| Title | Id | Backend | " + " | ".join(m[1] for m in MODES) + " |",
         "|---|---|---|" + ":-:|" * len(MODES)]
    for t in ts:
        L.append(f"| {cell(t['name'])} | `{t['title_id']}` | {t['backend']} | " + " | ".join(mark(t["modes"][k]["status"]) for k in MODE_KEYS) + " |")
    L += ["", "## Notes per title", ""]
    labels = {m[0]: m[1] for m in MODES}
    for t in ts:
        L += [f"### {cell(t['name'])} (`{t['title_id']}`)", "", f"Backend `{t['backend']}`: {BACKENDS[t['backend']]}.", "",
              "| Mode | Status | Note | Checked |", "|---|:-:|---|---|"]
        for k in MODE_KEYS:
            m = t["modes"][k]
            L.append(f"| {labels[k]} | {m['status']} | {cell(m['note'])} | {m['checked']} |")
        L.append("")
    L += ["## What the modes mean", "", "| Mode | Key | Meaning |", "|---|---|---|"]
    L += [f"| {label} | `{key}` | {cell(about)} |" for key, label, _, about in MODES]
    L += ["", "## Statuses and backends", "", "| Status | Meaning |", "|---|---|"]
    L += [f"| `{k}` | {v} |" for k, v in STATUSES.items()]
    L += ["", "| Backend | Meaning |", "|---|---|"]
    L += [f"| `{k}` | {v} |" for k, v in BACKENDS.items()]
    L += ["", "To update a status, edit `compat/online.json` (the status, a one-line note, the date checked) and run the generator. "
          "See also the [support matrix](support-matrix.md) for the offline state of each title.", ""]
    return "\n".join(L)


def render_readme_block(data):
    servers = data.get("servers", "the Napple servers")
    L = [BEGIN,
         f"**Online play** is between Napple players only, through the Napple-run servers at `{servers}`, never Nintendo's. "
         f"Generated from [`compat/online.json`](compat/online.json) (last checked {last_checked(data)}); what each mode means and the notes per title "
         "are in [online support](docs/reference/online-support.md).", "",
         "| Title | Backend | " + " | ".join(m[2] for m in MODES) + " |",
         "|---|---|" + ":-:|" * len(MODES)]
    for t in titles(data):
        name = cell(t["name"]) + (f" ({cell(t['short'])})" if t.get("short") else "")
        L.append(f"| {name} | {t['backend']} | " + " | ".join(mark(t["modes"][k]["status"]) for k in MODE_KEYS) + " |")
    L += ["", "`-` means the game has no such mode.", END]
    return "\n".join(L)


def splice_readme(text, block):
    i, j = text.find(BEGIN), text.find(END)
    if i < 0 or j < i or text.count(BEGIN) != 1 or text.count(END) != 1:
        sys.exit(f"{README}: needs exactly one {BEGIN} ... {END} block (in the Status section)")
    return text[:i] + block + text[j + len(END):]


def site_data(data):
    """The online table as one object: site/online.json, and the "online" part of site/compat.json (gen_site.py)."""
    return {"schema": 1, "generator": "tools/support/gen_online.py", "source": DATA, "servers": data.get("servers", ""),
            "last_checked": last_checked(data),
            "modes": [{"key": k, "label": label, "short": short, "about": about} for k, label, short, about in MODES],
            "statuses": STATUSES, "backends": BACKENDS,
            "titles": [{"title_id": t["title_id"], "name": t["name"], "short": t.get("short", ""), "backend": t["backend"],
                        "modes": {k: t["modes"][k] for k in MODE_KEYS}} for t in titles(data)]}


def render_site(data):
    return json.dumps(site_data(data), indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def build(root):
    data = load(root)
    readme = root / README
    outs = {DOC: render_doc(data), SITE: render_site(data)}
    if readme.exists():  # a scratch root without README (tests) only gets the doc and the site JSON
        outs[README] = splice_readme(readme.read_text(), render_readme_block(data))
    return outs


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    root = Path(a.root).resolve()
    outs = build(root)
    if a.check:
        stale = [p for p, t in outs.items() if not (root / p).exists() or (root / p).read_text() != t]
        for p in stale:
            print(f"FAIL {p}: stale or missing; run python3 -I tools/support/gen_support.py")
        print("online tables: " + ("OK" if not stale else f"{len(stale)} stale"))
        return 1 if stale else 0
    for p, t in outs.items():
        (root / p).parent.mkdir(parents=True, exist_ok=True)
        (root / p).write_text(t)
        print("wrote", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
