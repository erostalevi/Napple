#!/usr/bin/env python3
"""arbtrace.py FILE [--tid 0x51,...] [--addr 0x..] [--from T] [--to T] [--last N]
Prints a NAPPLE_ARB_TRACE ring (runtime/src/threads.c ArbRec, 56 bytes): time (s, CLOCK_MONOTONIC_RAW, the gfx CBTRACE clock), thread, svc, phase,
address(es), type/value/count, word before/after, waiters, result, woken threads."""
import argparse, struct, sys
REC = struct.Struct('<QQQIBBBBiiiiIHH')
NAMES = {0x1A: 'ArbLock', 0x1B: 'ArbUnlock', 0x1C: 'CvWait', 0x1D: 'CvSignal', 0x34: 'WaitAddr', 0x35: 'SignalAddr'}

def load(path):
    d = open(path, 'rb').read()
    out = [REC.unpack_from(d, o) for o in range(0, len(d) - REC.size + 1, REC.size)]
    out.sort(key=lambda r: r[0])
    return out

def fmt(r):
    t, a, a2, tid, svc, ph, ty, nw, val, cnt, bef, aft, res, w0, w1 = r
    s = '%.6f %#06x %-10s %s %#x' % (t / 1e9, tid, NAMES.get(svc, hex(svc)), 'exit ' if ph else 'enter', a)
    if a2: s += ' key %#x' % a2
    s += ' type %d val %d cnt %d word %d->%d waiters %d res %#x' % (ty, val, cnt, bef, aft, nw, res)
    if w0 or w1: s += ' woke %#x %#x' % (w0, w1)
    return s

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('file'); ap.add_argument('--tid'); ap.add_argument('--addr'); ap.add_argument('--from', dest='t0', type=float); ap.add_argument('--to', dest='t1', type=float)
    ap.add_argument('--last', type=int, default=0)
    a = ap.parse_args()
    recs = load(a.file)
    tids = {int(x, 16) for x in a.tid.split(',')} if a.tid else None
    addrs = {int(x, 16) for x in a.addr.split(',')} if a.addr else None
    sel = [r for r in recs if (not tids or r[3] in tids or r[13] in tids or r[14] in tids) and (not addrs or r[1] in addrs or r[2] in addrs)
           and (a.t0 is None or r[0] / 1e9 >= a.t0) and (a.t1 is None or r[0] / 1e9 <= a.t1)]
    for r in sel[-a.last:] if a.last else sel: print(fmt(r))
