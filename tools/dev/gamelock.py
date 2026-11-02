#!/usr/bin/env python3
"""gamelock.py -- run one command while holding one of the Mac-wide game slots, so several Claude sessions on one Mac never run more than N game processes at once (N = 3 by default, NAPPLE_GAME_SLOTS overrides it).

    python3 Napple/tools/dev/gamelock.py [--timeout SECONDS] -- <command> [args...]

A strict first-come queue: every caller takes a numbered ticket in /tmp/napple-mac-game.queue/ (timestamp-pid) and may only try the slot flocks
(/tmp/napple-mac-game.lock is slot 0, /tmp/napple-mac-game.lock.1 ... .N-1 the others) when its ticket is among the N oldest whose process is still alive. A process that re-takes the lock right after releasing it
(a long soak loop) therefore goes to the back of the line instead of starving everybody else. Tickets of dead processes are ignored and removed.
Prints how long it waited when that was more than a second. Exit status is the command's.
Every tool that runs a game (soak drivers, golden, e2e) must go through this script, one run slice at a time: do not hold the raw flock yourself."""
import fcntl, os, subprocess, sys, time

QDIR = "/tmp/napple-mac-game.queue"
LOCK = "/tmp/napple-mac-game.lock"
SLOTS = max(1, int(os.environ.get("NAPPLE_GAME_SLOTS", "3")))

def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True

def oldest_live(count):
    live = []
    for n in sorted(os.listdir(QDIR)):
        try:
            pid = int(n.rsplit("-", 1)[1])
        except (ValueError, IndexError):
            continue
        if alive(pid):
            live.append(n)
            if len(live) == count:
                break
            continue
        try:
            os.unlink(os.path.join(QDIR, n))
        except OSError:
            pass
    return live

def take_slot():
    for i in range(SLOTS):
        fd = os.open(LOCK if i == 0 else "%s.%d" % (LOCK, i), os.O_CREAT | os.O_RDWR, 0o666)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fd
        except OSError:
            os.close(fd)
    return None

def main():
    a = sys.argv[1:]
    timeout = None
    if a[:1] == ["--timeout"]:
        timeout, a = float(a[1]), a[2:]
    if a[:1] == ["--"]:
        a = a[1:]
    if not a:
        sys.exit("usage: gamelock.py [--timeout S] -- cmd args...")
    os.makedirs(QDIR, exist_ok=True)
    try:
        os.chmod(QDIR, 0o777)
    except OSError:
        pass
    ticket = "%020d-%d" % (time.time_ns(), os.getpid())
    tpath = os.path.join(QDIR, ticket)
    open(tpath, "w").close()
    fd = None
    t0 = time.time()
    try:
        while True:
            if ticket in oldest_live(SLOTS):
                fd = take_slot()
                if fd is not None:
                    break
            if timeout is not None and time.time() - t0 > timeout:
                sys.exit("gamelock: timed out waiting for the game lock")
            time.sleep(0.5)
        if time.time() - t0 > 1:
            print("gamelock: waited %.0f s for the game lock" % (time.time() - t0), file=sys.stderr)
        try:
            os.unlink(tpath)
        except OSError:
            pass
        sys.exit(subprocess.call(a))
    finally:
        try:
            os.unlink(tpath)
        except OSError:
            pass
        if fd is not None:
            os.close(fd)

if __name__ == "__main__":
    main()
