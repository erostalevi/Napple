#!/usr/bin/env python3
"""Guards "titles' known calls unchanged": every command row listed in known_calls_*.txt must still be in its service source with the same id and name.
Rows in known_calls_titles.txt (the commands LAS, SMO, ACNH and MK8D call) must also be IPC_IMPL, except the documented stubs in STUB_OK.
Usage: python3 -I tests/linux/check_known_calls.py   (from runtime/)"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "src" / "services"
row = re.compile(r'\{\s*(\d+)\s*,\s*"(\w+)"\s*,\s*\w+\s*,\s*IPC_(IMPL|STUB)')
name_only = re.compile(r'\{\s*(\d+)\s*,\s*"(\w+)"\s*,')
# a title may call these; the reply is a placeholder on purpose (reasons in the source comments next to each row)
STUB_OK = {("vi", "0", "TransactParcel"), ("vi", "3", "TransactParcelAuto")}
have = {}
status = {}
missing = []
notimpl = []
checked = 0
files = sorted(HERE.glob("known_calls_*.txt"))
for kc in files:
    for line in kc.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        f, cid, name = line.split()
        if f not in have:
            text = (SRC / f"{f}.c").read_text()
            have[f] = {(m.group(1), m.group(2)) for m in name_only.finditer(text)}
            status[f] = {}
            for m in row.finditer(text):
                status[f].setdefault((m.group(1), m.group(2)), set()).add(m.group(3))
        checked += 1
        if (cid, name) not in have[f]:
            missing.append(line)
        elif kc.name == "known_calls_titles.txt" and (f, cid, name) not in STUB_OK and status[f].get((cid, name)) == {"STUB"}:
            notimpl.append(line)
print(f'{{"test":"known_calls","files":{len(files)},"checked":{checked},"failed":{len(missing) + len(notimpl)}}}')
for m in missing:
    print("MISSING", m)
for m in notimpl:
    print("STUB (a title calls it)", m)
sys.exit(1 if missing or notimpl else 0)
