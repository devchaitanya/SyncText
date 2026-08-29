#!/usr/bin/env python3
"""Drive the built-in terminal editor over real PTYs and check live collaboration.

The TUI only activates on a terminal, so this test allocates a pty per editor
instead of redirecting to a file. It types one character at a time and asserts
that each keystroke -- not each save -- reaches the peer, and that the peer's
caret shows up on the other screen.

Prints one "RESULT PASS|FAIL <description>" line per assertion and exits
non-zero if any failed, so run_all_tests.sh can fold the results in.
"""
import os
import pty
import re
import subprocess
import sys
import threading
import time

BIN = "./build/editor"
DOC = "tuidoc"          # the document both editors open
USERS = ("t1", "t2")   # identities, supplied via SYNCTEXT_USER (normally automatic)
failures = 0


def result(ok, description):
    global failures
    if not ok:
        failures += 1
    print("RESULT %s %s" % ("PASS" if ok else "FAIL", description), flush=True)


def doc(user):
    try:
        with open("user_docs/%s/%s_doc.txt" % (DOC, user)) as f:
            return f.read()
    except OSError:
        return ""


def main():
    os.makedirs("user_docs/%s" % DOC, exist_ok=True)
    for user in USERS:
        with open("user_docs/%s/%s_doc.txt" % (DOC, user), "w") as f:
            f.write("alpha\n")

    # EDIT_WINDOW=0: this test covers the in-terminal editor, and must never open a
    # real terminal window on the machine running the suite.
    env = dict(os.environ, SYNCTEXT_TUI="1", SYNCTEXT_DEBUG_LEVEL="0",
               SYNCTEXT_STARTUP_SYNC="0", SYNCTEXT_EDIT_WINDOW="0")
    procs, masters = {}, {}
    for user in USERS:
        master, slave = pty.openpty()
        procs[user] = subprocess.Popen([BIN, DOC], stdin=slave, stdout=slave,
                                       stderr=subprocess.DEVNULL,
                                       env=dict(env, SYNCTEXT_USER=user), close_fds=True)
        os.close(slave)
        masters[user] = master
        time.sleep(0.6)

    # A real terminal drains its side of the pty constantly. If a test does not, the
    # buffer fills and the editor blocks in write() -- so read continuously on a
    # background thread and just accumulate what arrives.
    captured = {user: [] for user in USERS}
    draining = True

    def pump(user):
        while draining:
            try:
                chunk = os.read(masters[user], 65536)
            except OSError:
                return
            if not chunk:
                return
            captured[user].append(chunk)

    pumps = []
    for user in USERS:
        t = threading.Thread(target=pump, args=(user,), daemon=True)
        t.start()
        pumps.append(t)

    def drain(user):
        chunks, captured[user] = captured[user], []
        return b"".join(chunks).decode("utf-8", "replace")

    def send(user, text, per_key=0.12):
        for ch in text:
            os.write(masters[user], ch.encode())
            time.sleep(per_key)

    def wait_for(predicate, timeout=10.0, poll=0.1):
        """Wait for a condition instead of guessing a sleep length.

        Fixed sleeps made this test sensitive to machine load -- it would pass
        alone and fail at the end of a long suite. Polling keeps it fast when
        things are quick and patient when they are not.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            time.sleep(poll)
        return predicate()

    try:
        # Don't start typing until both editors have painted a frame.
        wait_for(lambda: all(captured[u] for u in USERS), timeout=15.0)
        time.sleep(0.5)
        drain("t1")
        drain("t2")

        # 1. per-character propagation, with no save anywhere in the loop
        send("t1", "hello")
        wait_for(lambda: "hello" in doc("t2") and doc("t1") == doc("t2"))
        result("hello" in doc("t2") and doc("t1") == doc("t2"),
               "Typed characters reach the peer without a save (t2: %r)" % doc("t2"))

        # 2. the peer's caret is visible and tracks the right column.
        # The peers bar reads "<dot> t1 <line>:<col>", 1-based like the rest of the
        # UI, so a caret after five typed characters shows as 1:6.
        carets = []

        def caret_seen():
            plain = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", drain("t2"))
            carets.extend(re.findall(r"t1 (\d+):(\d+)", plain))
            return bool(carets) and carets[-1] == ("1", "6")

        wait_for(caret_seen)
        result(bool(carets) and carets[-1] == ("1", "6"),
               "Peer caret rendered at the right position (saw %s)" % (carets[-1:] or "none"))

        # 3. Return splits the line and the split propagates
        os.write(masters["t1"], b"\r")
        time.sleep(0.3)
        send("t1", "world")
        wait_for(lambda: doc("t1") == doc("t2") and doc("t2").count("\n") == 2)
        result(doc("t1") == doc("t2") and doc("t2").count("\n") == 2,
               "Line split via Return propagated (t2: %r)" % doc("t2"))

        # 4. edits originating on the other side come back the other way
        send("t2", "XY")
        wait_for(lambda: doc("t1") == doc("t2") and "XY" in doc("t1"))
        result(doc("t1") == doc("t2") and "XY" in doc("t1"),
               "Peer's own typing propagated back (t1: %r)" % doc("t1"))

        # 5. backspace is an edit like any other
        before = doc("t1")
        os.write(masters["t1"], b"\x7f")
        wait_for(lambda: doc("t1") != before and doc("t1") == doc("t2"))
        result(doc("t1") != before and doc("t1") == doc("t2"),
               "Backspace propagated (t2: %r)" % doc("t2"))

        # 6. ^Q exits cleanly -- it must not be eaten as XON by the tty
        for user in USERS:
            os.write(masters[user], b"\x11")
        clean = True
        for user in USERS:
            try:
                procs[user].wait(timeout=6)
            except subprocess.TimeoutExpired:
                clean = False
        result(clean, "Ctrl-Q exits the editor cleanly")
    finally:
        draining = False
        for user in USERS:
            if procs[user].poll() is None:
                procs[user].kill()
            try:
                os.close(masters[user])
            except OSError:
                pass

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
