#!/usr/bin/env python3
"""End-to-end latency between real SyncText processes.

  keystroke -> peer : a key typed in one built-in editor (over a pty) until the
                      character is in the other participant's file.
  join              : a new participant starts until its file equals the session's.

Run from the repository root:  python3 tests/perf_e2e.py
"""
import os
import pty
import statistics
import subprocess
import threading
import time

BIN = "./build/editor"
BASE_ENV = dict(os.environ, SYNCTEXT_DEBUG_LEVEL="0", SYNCTEXT_STARTUP_SYNC="0",
                SYNCTEXT_EDIT_WINDOW="0", SYNCTEXT_NO_AUTO_OPEN="1")


def clean():
    subprocess.run("pkill -x editor; sleep 0.5; rm -f /dev/mqueue/synctext_* /dev/shm/synctext_*",
                   shell=True)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def start(args, user, tui, log=None):
    env = dict(BASE_ENV, SYNCTEXT_USER=user, SYNCTEXT_TUI="1" if tui else "0",
               SYNCTEXT_PORT_BASE="9471", SYNCTEXT_PORT_SPAN="1")
    if log:
        env["SYNCTEXT_DEBUG_LEVEL"] = "1"
    master, slave = pty.openpty()
    p = subprocess.Popen([BIN] + args, stdin=slave, stdout=slave,
                         stderr=open(log, "w") if log else subprocess.DEVNULL,
                         env=env, close_fds=True)
    os.close(slave)

    def pump():
        while True:
            try:
                if not os.read(master, 65536):
                    return
            except OSError:
                return
    threading.Thread(target=pump, daemon=True).start()
    return p, master


def stats(samples):
    s = sorted(samples)
    p95 = s[int(len(s) * 0.95) - 1]
    return "p50 %5.1f ms   p95 %5.1f ms   max %5.1f ms   (n=%d)" % (
        statistics.median(s), p95, s[-1], len(s))


def keystroke_latency(mode, n=60):
    clean()
    doc = "perf_keys"
    d = "user_docs/" + doc
    os.makedirs(d, exist_ok=True)
    for u in ("k1", "k2"):
        with open("%s/%s_doc.txt" % (d, u), "w") as f:
            f.write("x\n")
    if mode == "local":
        a = start([doc], "k1", True)
        time.sleep(0.6)
        b = start([doc], "k2", True)
    else:
        a = start(["--host", doc, "perfcode"], "k1", True)
        time.sleep(1.0)
        b = start(["--join", "127.0.0.1", "perfcode"], "k2", True)
    time.sleep(2.0)
    peer = "%s/k2_doc.txt" % d
    samples = []
    for i in range(n):
        ch = "abcdefghijklmnopqrstuvwxyz"[i % 26]
        before = read(peer).count(ch)
        t0 = time.perf_counter()
        os.write(a[1], ch.encode())
        while read(peer).count(ch) == before and time.perf_counter() - t0 < 2:
            time.sleep(0.0005)
        samples.append((time.perf_counter() - t0) * 1000)
        time.sleep(0.12)
    for p, _ in (a, b):
        p.terminate()
    return samples


def join_time(mode, size_kb):
    """Process start until the joiner's replica holds the shared history ("[sync] joined")."""
    clean()
    doc = "perf_join"
    d = "user_docs/" + doc
    subprocess.run("rm -rf " + d, shell=True)
    os.makedirs(d, exist_ok=True)
    text = "".join("line %06d %s\n" % (i, "y" * 28) for i in range(size_kb * 1024 // 40))
    with open(d + "/j1_doc.txt", "w") as f:
        f.write(text)
    log = "log_files/perf_join.err"
    if mode == "local":
        a = start([doc], "j1", False)
        time.sleep(1.0)
        t0 = time.perf_counter()
        b = start([doc], "j2", False, log)
    else:
        a = start(["--host", doc, "perfcode"], "j1", False)
        time.sleep(1.0)
        t0 = time.perf_counter()
        b = start(["--join", "127.0.0.1", "perfcode"], "j2", False, log)
    while "[sync] joined" not in read(log) and time.perf_counter() - t0 < 30:
        time.sleep(0.001)
    joined = (time.perf_counter() - t0) * 1000
    ok = "[sync] joined" in read(log)
    time.sleep(0.3)
    same = read(d + "/j2_doc.txt") == text
    for p, _ in (a, b):
        p.terminate()
    os.remove(log)
    return joined if ok else float("nan"), same


def main():
    print("== Keystroke -> peer's file (built-in editor, 60 keystrokes) ==")
    for mode in ("local", "lan"):
        print("  %-5s %s" % (mode, stats(keystroke_latency(mode))))
    print("== Joining a running session (process start -> file synced) ==")
    for mode in ("local", "lan"):
        for kb in (10, 100, 500):
            ms, same = join_time(mode, kb)
            print("  %-5s %4d KB doc: %6.0f ms to receive the full history (file identical: %s)" %
                  (mode, kb, ms, "yes" if same else "NO"))
    clean()
    subprocess.run("rm -rf user_docs/perf_keys user_docs/perf_join", shell=True)


if __name__ == "__main__":
    main()
