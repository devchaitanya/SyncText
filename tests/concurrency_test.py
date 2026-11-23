#!/usr/bin/env python3
"""Concurrent editing in the built-in editor, over real PTYs.

Several editors type into the same document at the same moments. With the CRDT,
every copy must end up byte-for-byte identical and keep every keystroke; the
checks below also pin down *where* concurrent text lands.

Prints one "RESULT PASS|FAIL <description>" line per assertion (the format
run_all_tests.sh folds in) and exits non-zero if any failed.
"""
import os
import pty
import subprocess
import sys
import threading
import time

BIN = "./build/editor"
HOME, END, RIGHT, ENTER, BACKSPACE, QUIT = b"\x1b[H", b"\x1b[F", b"\x1b[C", b"\r", b"\x7f", b"\x11"
failures = 0


def result(ok, description, detail=""):
    global failures
    if not ok:
        failures += 1
    print("RESULT %s %s" % ("PASS" if ok else "FAIL", description) +
          ("" if ok or not detail else "  (%s)" % detail), flush=True)


class Session:
    """A few editors on one document, each on its own pty."""

    def __init__(self, doc, users, initial):
        self.doc, self.users = doc, users
        os.makedirs("user_docs/" + doc, exist_ok=True)
        for u in users:
            with open(self.path(u), "w") as f:
                f.write(initial)
        env = dict(os.environ, SYNCTEXT_TUI="1", SYNCTEXT_DEBUG_LEVEL="0", SYNCTEXT_STARTUP_SYNC="0",
                   SYNCTEXT_EDIT_WINDOW="0", SYNCTEXT_NO_AUTO_OPEN="1")
        self.procs, self.masters = {}, {}
        for u in users:
            master, slave = pty.openpty()
            self.procs[u] = subprocess.Popen([BIN, doc], stdin=slave, stdout=slave,
                                             stderr=subprocess.DEVNULL,
                                             env=dict(env, SYNCTEXT_USER=u), close_fds=True)
            os.close(slave)
            self.masters[u] = master
            threading.Thread(target=self._pump, args=(master,), daemon=True).start()
            time.sleep(0.6)
        time.sleep(1.0)                      # let the later ones join

    def _pump(self, fd):                     # keep each pty drained so the editors never block
        while True:
            try:
                if not os.read(fd, 65536):
                    return
            except OSError:
                return

    def path(self, u):
        return "user_docs/%s/%s_doc.txt" % (self.doc, u)

    def text(self, u):
        try:
            with open(self.path(u)) as f:
                return f.read()
        except OSError:
            return ""

    def keys(self, u, data):
        os.write(self.masters[u], data)

    def settle(self, timeout=8.0):
        """Wait until every copy is identical; return that text (or None)."""
        time.sleep(0.2)                        # let the last keystrokes reach the files
        deadline = time.time() + timeout
        while time.time() < deadline:
            texts = {self.text(u) for u in self.users}
            if len(texts) == 1:
                first = texts.pop()
                time.sleep(0.5)                # and stays the same text, unchanged
                if {self.text(u) for u in self.users} == {first}:
                    return first
            time.sleep(0.1)
        return None

    def close(self):
        for u in self.users:
            try:
                self.keys(u, QUIT)
            except OSError:
                pass
        time.sleep(1.0)
        for p in self.procs.values():
            if p.poll() is None:
                p.kill()
        for p in self.procs.values():
            p.wait()


def simultaneous(session, plan, rounds, gap=0.03):
    """plan: {user: bytes per round}; all users press their key in the same instant."""
    for _ in range(rounds):
        for u, data in plan.items():
            session.keys(u, data)
        time.sleep(gap)


def test_same_line_both_ends():
    s = Session("cc_ends", ("t1", "t2"), "hello world\n")
    s.keys("t1", END)
    s.keys("t2", HOME)
    time.sleep(0.3)
    simultaneous(s, {"t1": b"A", "t2": b"B"}, 6)
    final = s.settle()
    s.close()
    result(final is not None, "Two editors typing on one line converge", repr(final))
    result(final == "BBBBBBhello worldAAAAAA\n",
           "Both editors' keystrokes are kept, at the line's start and end", repr(final))


def test_same_spot_runs_do_not_interleave():
    s = Session("cc_spot", ("t1", "t2"), "ab\n")
    for u in ("t1", "t2"):
        s.keys(u, HOME + RIGHT)            # both carets between 'a' and 'b'
    time.sleep(0.3)
    for i in range(5):                     # each types its own word at the same spot
        s.keys("t1", b"xyzuv"[i:i + 1])
        s.keys("t2", b"12345"[i:i + 1])
        time.sleep(0.03)
    final = s.settle()
    s.close()
    result(final is not None, "Typing at the same spot converges", repr(final))
    result(final in ("axyzuv12345b\n", "a12345xyzuvb\n"),
           "Words typed at the same spot stay whole (no interleaving)", repr(final))


def test_three_editors_structure_and_text():
    s = Session("cc_three", ("t1", "t2", "t3"), "one\ntwo\nthree\n")
    s.keys("t1", HOME)                     # t1: new lines above "one"
    s.keys("t2", b"\x1b[B" + END)          # t2: end of "two"
    s.keys("t3", b"\x1b[B\x1b[B" + END)    # t3: end of "three", deleting
    time.sleep(0.3)
    simultaneous(s, {"t1": b"X" + ENTER, "t2": b"!", "t3": BACKSPACE}, 3)
    final = s.settle()
    s.close()
    result(final is not None, "Three editors (new lines, typing, deleting) converge", repr(final))
    result(final == "X\nX\nX\none\ntwo!!!\nth\n",
           "Line inserts, typing and deletes from three editors all apply", repr(final))


def test_caret_follows_its_character():
    s = Session("cc_caret", ("t1", "t2"), "0123456789\n")
    s.keys("t1", HOME + RIGHT * 6)          # t1 caret between '5' and '6'
    time.sleep(0.3)
    s.keys("t2", HOME + b"XX")              # t2 shifts the whole line right
    time.sleep(1.0)
    s.keys("t1", b"Z")                      # lands next to '5', not two chars early
    final = s.settle()
    s.close()
    result(final == "XX012345Z6789\n",
           "A caret stays next to its character when a peer types before it", repr(final))


def main():
    for test in (test_same_line_both_ends, test_same_spot_runs_do_not_interleave,
                 test_three_editors_structure_and_text, test_caret_follows_its_character):
        test()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
