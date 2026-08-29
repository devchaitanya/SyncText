#!/usr/bin/env python3
"""Editor in its own window, monitor in the original terminal.

SYNCTEXT_EDIT_TTY points the editor at another terminal, which is how the feature
is tested without a display (and how you pair it with tmux or a second ssh
session). The window opened by SYNCTEXT_EDIT_WINDOW uses exactly this path once
the helper reports its tty.

Prints "RESULT PASS|FAIL <description>" lines for run_all_tests.sh.
"""
import os
import pty
import re
import subprocess
import sys
import threading
import time

BIN = "./build/editor"
DOC = "windoc"        # document both sessions open

failures = 0
stop = threading.Event()
cap = {}


def result(ok, description):
    global failures
    if not ok:
        failures += 1
    print("RESULT %s %s" % ("PASS" if ok else "FAIL", description), flush=True)


def pump(name, fd):
    cap[name] = []

    def run():
        while not stop.is_set():
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            cap[name].append(chunk)

    threading.Thread(target=run, daemon=True).start()


def text(name):
    return b"".join(cap[name]).decode("utf-8", "replace")


def plain(name):
    return re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text(name))


def doc(user):
    try:
        with open("user_docs/%s/%s_doc.txt" % (DOC, user)) as f:
            return f.read()
    except OSError:
        return ""


def main():
    os.makedirs("user_docs/%s" % DOC, exist_ok=True)
    with open("user_docs/%s/e1_doc.txt" % DOC, "w") as f:
        f.write("first line\nsecond line\n")
    with open("user_docs/%s/e2_doc.txt" % DOC, "w") as f:
        f.write("first line\nsecond line\n")

    main_m, main_s = pty.openpty()
    edit_m, edit_s = pty.openpty()
    edit_path = os.ttyname(edit_s)
    pump("main", main_m)
    pump("edit", edit_m)

    # EDIT_TTY names the "window" explicitly, so no real terminal is ever spawned.
    env = dict(os.environ, SYNCTEXT_TUI="1", SYNCTEXT_EDIT_TTY=edit_path,
               SYNCTEXT_EDIT_WINDOW="1")
    p1 = subprocess.Popen([BIN, DOC], stdin=main_s, stdout=main_s,
                          stderr=subprocess.DEVNULL,
                          env=dict(env, SYNCTEXT_USER="e1"), close_fds=True)
    os.close(main_s)
    time.sleep(2.0)

    peer_m, peer_s = pty.openpty()
    pump("peer", peer_m)
    p2 = subprocess.Popen([BIN, DOC], stdin=peer_s, stdout=peer_s,
                          stderr=subprocess.DEVNULL,
                          env=dict(os.environ, SYNCTEXT_TUI="1", SYNCTEXT_EDIT_WINDOW="0",
                                   SYNCTEXT_USER="e2"),
                          close_fds=True)
    os.close(peer_s)
    time.sleep(2.0)

    try:
        for ch in "TYPED ":
            os.write(edit_m, ch.encode())
            time.sleep(0.06)
        time.sleep(2.0)

        result("TYPED" in doc("e1"),
               "Typing in the editor window edits the document (e1: %r)" % doc("e1"))
        result(doc("e2") == doc("e1"),
               "Edits from the editor window reach peers (e2: %r)" % doc("e2"))

        edit_screen = plain("edit")
        main_screen = plain("main")
        result("^Q quit" in edit_screen,
               "Editor UI is drawn in the separate window")
        result("Active users" in main_screen and "^Q quit" not in main_screen,
               "Original terminal shows the monitor view, not the editor")
        result(re.search(r"e2 \d+:\d+", edit_screen) is not None,
               "Peer caret is shown in the editor window")

        os.write(edit_m, b"\x11")
        clean = True
        try:
            p1.wait(timeout=8)
        except subprocess.TimeoutExpired:
            clean = False
        result(clean, "Ctrl-Q in the editor window exits cleanly")
    finally:
        stop.set()
        for proc in (p1, p2):
            if proc.poll() is None:
                proc.kill()
        for fd in (main_m, edit_m, peer_m, edit_s):
            try:
                os.close(fd)
            except OSError:
                pass

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
