#!/usr/bin/env python3
"""Guards "titles' known calls unchanged": every command row listed in known_calls_ipc_depth.txt must still be in its service source with the same id and name.
Usage: python3 -I tests/linux/check_known_calls.py   (from runtime/)"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "src" / "services"
row = re.compile(r'\{\s*(\d+)\s*,\s*"(\w+)"\s*,')
have = {}
missing = []
checked = 0
for line in (HERE / "known_calls_ipc_depth.txt").read_text().splitlines():
    if not line or line.startswith("#"):
        continue
    f, cid, name = line.split()
    if f not in have:
        have[f] = {(m.group(1), m.group(2)) for m in row.finditer((SRC / f"{f}.c").read_text())}
    checked += 1
    if (cid, name) not in have[f]:
        missing.append(line)
print(f'{{"test":"known_calls_ipc_depth","checked":{checked},"failed":{len(missing)}}}')
for m in missing:
    print("MISSING", m)
sys.exit(1 if missing else 0)
