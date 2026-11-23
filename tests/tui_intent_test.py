#!/usr/bin/env python3
"""Concurrent edits in the built-in editor land where the user meant them.

A text diff only sees "before" and "after", so in repeated text it can pick the
wrong characters: deleting the first "BCD" of "ABCDBCDBCD" looks exactly like
deleting the last one. If a peer concurrently inserted inside the "BCD" that was
really kept, a wrong guess moves the peer's text. The editor knows which
characters the key removed, so it must send exactly those.

To make the two edits concurrent, alice's process is paused (SIGSTOP) while bob
edits and while alice's keys are queued in her terminal; on SIGCONT she applies
her keys before she reads bob's change.

Prints one "RESULT PASS|FAIL <description>" line per case and exits non-zero if
any failed, so run_all_tests.sh can fold the results in.
"""
import os
import pty
import signal
import subprocess
import sys
import threading
import time

BIN = "./build/editor"
RIGHT, HOME, DELETE, BACKSPACE = b"\x1b[C", b"\x1b[H", b"\x1b[3~", b"\x7f"
failures = 0


def result(ok, description):
    global failures
    if not ok:
        failures += 1
    print("RESULT %s %s" % ("PASS" if ok else "FAIL", description), flush=True)


def wait_for(predicate, timeout=10.0, poll=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    return predicate()


def run_case(doc, text, alice_col, alice_keys, bob_col, bob_keys, expected, description):
    users = ("ia", "ib")
    os.makedirs("user_docs/%s" % doc, exist_ok=True)
    for user in users:
        with open("user_docs/%s/%s_doc.txt" % (doc, user), "w") as f:
            f.write(text + "\n")

    def read(user):
        try:
            with open("user_docs/%s/%s_doc.txt" % (doc, user)) as f:
                return f.read()
        except OSError:
            return ""

    env = dict(os.environ, SYNCTEXT_TUI="1", SYNCTEXT_DEBUG_LEVEL="0",
               SYNCTEXT_STARTUP_SYNC="0", SYNCTEXT_EDIT_WINDOW="0")
    procs, masters, painted = {}, {}, {}
    draining = [True]

    def pump(user):
        # A real terminal drains its pty; if nobody does, the editor blocks in write().
        while draining[0]:
            try:
                chunk = os.read(masters[user], 65536)
            except OSError:
                return
            if not chunk:
                return
            painted[user] = True

    try:
        for user in users:                    # ia starts first and founds the session
            master, slave = pty.openpty()
            procs[user] = subprocess.Popen([BIN, doc], stdin=slave, stdout=slave,
                                           stderr=subprocess.DEVNULL,
                                           env=dict(env, SYNCTEXT_USER=user), close_fds=True)
            os.close(slave)
            masters[user] = master
            threading.Thread(target=pump, args=(user,), daemon=True).start()
            time.sleep(0.6)

        def keys(user, seq, gap=0.08):
            for k in seq:
                os.write(masters[user], k)
                time.sleep(gap)

        wait_for(lambda: all(painted.get(u) for u in users), timeout=15.0)
        wait_for(lambda: read("ib") == text + "\n", timeout=10.0)
        time.sleep(0.8)                       # both synced and idle

        # Put both carets in place while everyone is still talking to each other.
        keys("ia", [HOME] + [RIGHT] * alice_col)
        keys("ib", [HOME] + [RIGHT] * bob_col)
        time.sleep(0.5)

        # Concurrent edits: alice is frozen while bob edits and while her own keys
        # queue up in her terminal, so neither has seen the other's change.
        os.kill(procs["ia"].pid, signal.SIGSTOP)
        try:
            keys("ib", bob_keys)
            wait_for(lambda: read("ib") != text + "\n", timeout=5.0)
            time.sleep(0.3)
            os.write(masters["ia"], b"".join(alice_keys))
            time.sleep(0.2)
        finally:
            os.kill(procs["ia"].pid, signal.SIGCONT)

        want = expected + "\n"
        wait_for(lambda: read("ia") == want and read("ib") == want, timeout=10.0)
        got_a, got_b = read("ia"), read("ib")
        result(got_a == want and got_b == want,
               "%s (want %r, alice %r, bob %r)" % (description, expected, got_a.rstrip("\n"),
                                                   got_b.rstrip("\n")))
    finally:
        draining[0] = False
        for user, proc in procs.items():
            if proc.poll() is None:
                try:
                    os.write(masters[user], b"\x11")      # ^Q
                    proc.wait(timeout=4)
                except (OSError, subprocess.TimeoutExpired):
                    proc.kill()
            try:
                os.close(masters[user])
            except OSError:
                pass
        time.sleep(0.5)


def main():
    # alice deletes the FIRST "BCD" (caret after A, three Deletes); bob concurrently
    # types X after the THIRD B. A diff would record the last "BCD" as deleted and
    # leave X stranded after the deleted text.
    run_case("intent1", "ABCDBCDBCD", 1, [DELETE] * 3, 8, [b"X"], "ABCDBXCD",
             "Deleting the first of three repeated blocks keeps a concurrent insert in place")
    # In a run of equal characters: alice backspaces the first 'a', bob concurrently
    # types X after it. A diff would delete the second 'a' instead.
    run_case("intent2", "aa", 1, [BACKSPACE], 1, [b"X"], "Xa",
             "Backspace in a run of equal characters deletes the character before the caret")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
