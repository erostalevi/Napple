#!/usr/bin/env python3
"""gamelock.py -- run one command while holding the Mac-wide game lock, so several Claude sessions on one Mac never run two game processes at once.

    python3 Napple/tools/dev/gamelock.py [--timeout SECONDS] -- <command> [args...]

A strict first-come queue: every caller takes a numbered ticket in /tmp/napple-mac-game.queue/ (timestamp-pid) and may only try the flock on
/tmp/napple-mac-game.lock when its ticket is the oldest one whose process is still alive. A process that re-takes the lock right after releasing it
(a long soak loop) therefore goes to the back of the line instead of starving everybody else. Tickets of dead processes are ignored and removed.
Prints how long it waited when that was more than a second. Exit status is the command's.
Every tool that runs a game (soak drivers, golden, e2e) must go through this script, one run slice at a time: do not hold the raw flock yourself."""
import fcntl, os, subprocess, sys, time

QDIR = "/tmp/napple-mac-game.queue"
LOCK = "/tmp/napple-mac-game.lock"

def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True

def oldest_live():
    for n in sorted(os.listdir(QDIR)):
        try:
            pid = int(n.rsplit("-", 1)[1])
        except (ValueError, IndexError):
            continue
        if alive(pid):
            return n
        try:
            os.unlink(os.path.join(QDIR, n))
        except OSError:
            pass
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
    fd = os.open(LOCK, os.O_CREAT | os.O_RDWR, 0o666)
    t0 = time.time()
    try:
        while True:
            if oldest_live() == ticket:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except OSError:
                    pass
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
        os.close(fd)

if __name__ == "__main__":
    main()
