# SyncText — CRDT-Based Collaborative Text Editor

SyncText is a lightweight collaborative editor that discovers peers via a shared-memory registry and exchanges edits over POSIX message queues. It maintains convergence using a pragmatic CRDT-style merge with Last-Writer-Wins arbitration.

## Build

Requirements — **Linux only** (see [Platform support](#platform-support); Windows works
via WSL2, macOS does not build):
- g++ (C++17)
- Linux kernel interfaces: inotify, POSIX message queues, eventfd, POSIX shared memory

```bash
make
```

> **Collaborating with someone on another machine?** Go straight to
> [Testing with a second machine, step by step](#testing-with-a-second-machine-step-by-step).
> Build on both machines, then one runs `--host` and the other runs `--join`.

## Quick Test

Run the comprehensive test suite:
```bash
./run_all_tests.sh
```
This runs 15 test groups (44 assertions):
- Multi-line paste (13 lines)
- Mass insert (50 lines)
- Delete-all operation
- Non-conflicting edits
- Conflicting edits (LWW)
- Structural line insert
- Structural line delete
- LAN mode over TCP (snapshot on connect, host/joiner relay, access-code rejection)
- LAN host failover (roster propagation, promotion of the next survivor, cascading failover)
- Cross-directory startup sync (a session started from another working directory)
- Separate editor window (editor on one terminal, monitor on the other)
- Built-in terminal editor (per-keystroke propagation, peer carets, Return/Backspace, clean exit)
- Separate documents by name, and rejection of unsafe names
- The access code selecting one shared document out of several
- Automatic identities (a second session of one account, and a host renaming a joiner)

The terminal-editor test drives two editors over real PTYs via
`tests/tui_pty_test.py`, and is skipped if `python3` is unavailable.

The LAN test binds `127.0.0.1:9411` by default; override with `LAN_PORT=<port> ./run_all_tests.sh`
if that port is in use.

Individual tests are in the `tests/` directory.

## Run

### Built-in terminal editor

Running in a terminal drops you straight into SyncText's own editor. This is the
default because an external application only reports a change when you *save*, which
makes per-character updates impossible and leaves nowhere to draw a collaborator's
caret. Editing in the terminal means every keystroke is diffed and broadcast as you
type, and every peer's caret is drawn in the document in its own colour.

```
 SyncText  alice                              alice_doc.txt  6 lines   <- status bar
   1 │ #include <stdio.h>
   2 │
   3 │ int main(void) {
 ▸ 4 │     printf("hello, world\n");                                   <- active line
   5 │     return 0;
   6 │ }
 ~
 ● bob 4:8    ● carol 2:1                              Ln 4  Col 5     <- peers + position
 ^Q quit   ^S flush   ^L redraw   arrows/Home/End/PgUp/PgDn move
```

The active line is banded and its number highlighted; each peer gets a stable colour,
used both for the dot in the status bar and for the cell their caret sits on in the
text. Line and column numbers are 1-based. Long lines scroll horizontally and are
marked with `›`. Set `SYNCTEXT_ASCII=1` to swap the box-drawing glyphs for plain
ASCII, or `SYNCTEXT_NO_COLOR=1` to drop colour entirely.

| Key | Action |
| --- | --- |
| printable keys, Return, Backspace, Delete | edit the document |
| arrows, Home, End, PgUp, PgDn | move the caret |
| `Ctrl-Q` | quit |
| `Ctrl-S` | flush pending updates now |
| `Ctrl-L` | force a redraw |

The caret of each peer is highlighted in the text at its exact line and column, and
listed in the footer. A peer's caret disappears if nothing is heard from it for 15
seconds.

#### Convergence limits when two people type at once

The merge is Last-Writer-Wins at line granularity, not a character-level CRDT. Two
people editing *different* lines, or taking turns on the same line, converge reliably.
Two people typing into the **same line at the same time** do not: each local keystroke
stamps that line's timestamp, so incoming remote edits older than the stamp are dropped
as stale, and the two copies can stay diverged after the typing stops.

This is a property of the merge algorithm, not of the terminal editor -- the same
divergence is reproducible through the file-watch path with no editor involved. The
built-in editor makes it easier to hit, because it publishes every keystroke rather
than every save. Converging that case properly needs a character-level CRDT (RGA or
similar), which is a redesign of the merge stage rather than a fix.

#### A window of its own

By default the editor opens in **its own terminal window**, leaving the terminal you
started in as a live status view — active users, recent changes, and the monitor
hotkeys:

```
 terminal you started in            editor window
 ┌──────────────────────────┐       ┌──────────────────────────────┐
 │ Document: alice_doc.txt  │       │  SyncText  alice             │
 │ Line 0: int x = 50;      │       │    1 │ int x = 50;           │
 │ Active users: alice, bob │       │  ▸ 2 │ int y = 20;           │
 │ [q] quit  [o] open       │       │  ● bob 1:9        Ln 2 Col 5 │
 │ Monitoring for changes...│       │  ^Q quit  ^S flush           │
 └──────────────────────────┘       └──────────────────────────────┘
```

This is still one process: rather than run a second instance (which would need its own
registry slot and message queue for what is one participant), SyncText opens a window,
learns its tty, and simply draws the editor there. Peer carets and per-keystroke sync
work exactly as they do in-terminal.

- `SYNCTEXT_EDIT_WINDOW=0` keeps everything in one terminal.
- `SYNCTEXT_TERMINAL="kitty -e"` picks the terminal emulator; otherwise gnome-terminal,
  konsole, xfce4-terminal, alacritty, kitty, and xterm are tried in turn.
- `SYNCTEXT_EDIT_TTY=/dev/pts/7` uses a terminal **you** already have open instead of
  opening one — run `tty` in the target terminal to get its path. This is how to pair
  the editor with a tmux pane or a second ssh session.

If no window can be opened (no display, no terminal emulator found), the editor simply
stays in the current terminal.

The editor activates when **both** stdin and stdout are terminals. Redirect either one
— as the test suite does — and SyncText falls back to the monitor view plus an external
editor. Force the choice with `SYNCTEXT_TUI=1` or `SYNCTEXT_TUI=0`.

With the built-in editor off, SyncText opens your document in a GUI editor instead
(`SYNCTEXT_EDITOR`, then `$VISUAL`/`$EDITOR`, then gedit/gnome-text-editor/code, then
`xdg-open`); `SYNCTEXT_NO_AUTO_OPEN=1` disables that. In that mode changes are only
detected on save.

### Change detection

The program uses inotify for near-instant change detection (typ. <1s) and falls back to ~1s polling if inotify isn't available.

Tuning idle behavior:
- Spinner is OFF by default. Enable with `SYNCTEXT_SPINNER=1` for a 1s idle redraw heartbeat.
- `SYNCTEXT_IDLE_REFRESH_SEC=10` adjusts the idle poll timeout (used when spinner is off and/or as a fallback). Larger values reduce wakeups.
- `SYNCTEXT_PERIODIC_MTIME=1` enables a periodic file mtime check on each idle timeout even when inotify is available (off by default). This can help on unusual setups where file writes don’t emit inotify events.

If your terminal looks odd after an abrupt kill (e.g., via `timeout`), restore it with:
```bash
stty sane
# or
reset
```

Quick start:
```bash
./build/editor notes                    # open (or create) a document called "notes"
./build/editor --host notes hunter2     # ...and share it with the LAN
```

### Command-line Arguments

```bash
./build/editor <doc>                     # open a document on this machine
./build/editor --host <doc> <code>       # open it and share it
./build/editor --join <host_ip> <code>   # join a shared document
```

That is the whole interface. There is no user id and no port to choose:

-   **`<doc>`** — the document's name. Its files live in `user_docs/<doc>/`. Opening the
    same name again reopens the same document; a name that does not exist yet is
    created.
-   **`<code>`** — the shared password for that document. It is also what a joiner
    presents, so it is what *selects* the document: give each shared document its own
    code.
-   **Your identity** comes from your OS account. Two sessions from one account on one
    machine (or a host and joiner on the same machine) are renamed automatically —
    `chaitu`, `chaitu-2` — so participants never collide.
-   **The port** is chosen automatically: a host takes the first free one from 9000 up
    and prints it, and a joiner finds it by scanning.

Everything else is an environment variable rather than an argument, so the common case
stays short. `SYNCTEXT_USER` fixes the identity (scripts and the test suite use it),
`SYNCTEXT_PORT_BASE` / `SYNCTEXT_PORT_SPAN` move or resize the port range, and
`SYNCTEXT_MERGE_BATCH_N` sets the merge batch size.

#### Working on more than one document

Documents are separate by name. Run the editor once per document — different terminals,
different documents, no coordination:

```bash
./build/editor notes        # terminal 1
./build/editor plan         # terminal 2
```

Each has its own directory, its own registry and its own message queues, so they never
see each other:

| | `notes` | `plan` |
| --- | --- | --- |
| files | `user_docs/notes/<you>_doc.txt` | `user_docs/plan/<you>_doc.txt` |
| registry | `/dev/shm/synctext_reg_notes` | `/dev/shm/synctext_reg_plan` |
| queues | `/synctext_notes_<you>` | `/synctext_plan_<you>` |

Names may contain letters, digits, `-`, `_` and `.`; anything else is rejected rather
than rewritten, so `a/b` cannot silently become the same document as `a_b`. `.` and `..`
are refused, and names are capped at 64 characters. A name longer than 21 characters
keeps its full form for the directory but uses a shortened, hashed form inside POSIX
queue names, which are capped at 64 bytes.

Each document leaves a small registry in `/dev/shm` (about 3 KB), not removed when the
last session exits — unlinking it while another process is registering would split the
registry in two, which is worse than the leak. Reclaim them when nothing is running:

```bash
rm -f /dev/shm/synctext_reg_* /dev/mqueue/synctext_*
```

#### Sharing, and how a joiner finds the right document

Share a document by adding a code. The host prints exactly what to pass on:

```
$ ./build/editor --host notes hunter2
Hosting on port 9000 (chosen automatically)
  Others join with:  ./build/editor --join <this machine's IP> hunter2
```

A joiner gives only the address and the code. The document name, the port and its own
identity all come back from the host:

```bash
./build/editor --join 192.168.1.25 hunter2
```

Share several documents at once by running one host each, with **different codes**. The
joiner's code is what picks the document, so nobody has to track port numbers:

```bash
./build/editor --host notes codeN     # takes port 9000
./build/editor --host plan  codeP     # takes port 9001
./build/editor --join 192.168.1.25 codeP    # scans, matches codeP, joins "plan"
```

A joiner tries each port in the range and stops at the host whose code it matches, so a
wrong code is reported rather than silently joining the wrong document. Scanning does
present the code to every port it tries, so on an untrusted network set
`SYNCTEXT_PORT_SPAN=1` with a known `SYNCTEXT_PORT_BASE`.

#### Advanced: one process holding several documents (`--serve`)

`--host` shares the document that process is editing. `--serve` is a headless process
that holds several documents on one port and relays between their participants without
editing any of them:

```bash
./build/editor --serve topsecret --doc notes --doc plan
```

Participants name the document they want, since one code covers all of them:

```bash
./build/editor --doc notes --join 192.168.1.25 topsecret
```

You do not need this for ordinary use — running one `--host` per document does the same
job with automatic ports. Two things to know if you do use it: the server keeps a
replica per document that is updated **in arrival order without the full merge**, so a
joiner arriving mid-session can be seeded with a slightly stale copy; and a server is
not an editor, so stopping it ends the session for everyone on that port.

#### Testing with a second machine, step by step

Both machines on the same WiFi. **A** hosts, **B** is your friend.

**1. Build on both machines.** Each runs its own binary.

```bash
make                      # needs g++ with C++17 and Linux (POSIX shm, mq, inotify)
```

**2. On A, share a document.** No user id, no port — just a name and a code:

```bash
./build/editor --host notes hunter2
```

It prints the line to send to B:

```
Hosting on port 9000 (chosen automatically)
  Others join with:  ./build/editor --join <this machine's IP> hunter2
```

Get A's address with `hostname -I | awk '{print $1}'`. If A runs a firewall, open the
range once:

```bash
sudo ufw status                        # skip if this says "inactive"
sudo ufw allow 9000:9015/tcp
```

**3. On B, join.**

```bash
./build/editor --join 192.168.1.25 hunter2
```

B does not name the document — it comes from A, along with B's identity, so B's copy
lands in `user_docs/notes/` automatically.

**4. What success looks like.** B's screen fills with A's document and both list each
other:

```
Active users  ● chaitu (host)   ● chaitu-2
```

Type on either machine. Characters appear on the other as they are typed, and each
caret is drawn in the text in its own colour.

**5. Quitting.** `Ctrl-Q`. If A quits or crashes the session survives — B is promoted
to host and the bar shows `[host gen 1]`. To end it entirely, quit everyone.

##### Before you start, three things worth knowing

- **B's copy of that document is overwritten** on connect with A's. If B already has a
  `notes` document worth keeping, join under a different document name on A's side, or
  back it up.
- **The access code crosses the network in clear text.** It keeps strangers on the WiFi
  out; it is not encryption. Don't reuse a real password.
- **Editing the same line at the same time does not converge** — see the note above.
  Different lines are fine.

#### If the join fails

The message names the cause; each one fails within about ten seconds rather than
hanging:

| Message | What to do |
| --- | --- |
| `Host rejected the connection: invalid access code` | You reached A. The codes differ — retype both, watch for shell history or quoting. |
| `Could not reach <ip>:<port> ...` | Nothing answered. Check A is running `--host`, the address from `hostname -I` is current, both are on the same network, and A's firewall allows the port. |
| `... accepted the connection but sent no reply within Nms` | Something is listening on that port, but it is not a SyncText host. Try another port on both sides. |
| `Connected to this machine's own listener ...` | `--join` was pointed at B itself. Use A's address. |
| `No session on <ip> ports 9000-9015 accepted this request` | Nothing in the scanned range matched that code. Check the code, and that A is still hosting. |
| `'<name>' is not a valid IPv4 address` | Hostnames are not resolved — use the numeric address. |
| `Could not listen on port N ...` | On A: something already holds that port. Pick another and tell B. |

Quick reachability check from B before blaming SyncText:

```bash
ping -c2 192.168.1.25                 # is A reachable at all?
nc -vz 192.168.1.25 9000              # is the port open? (needs A already hosting)
```

Both connect attempts and handshakes are bounded by `SYNCTEXT_CONNECT_TIMEOUT_MS`
(default `10000`). Raise it on a slow link:

```bash
SYNCTEXT_CONNECT_TIMEOUT_MS=20000 ./build/editor --join 192.168.1.25 supersecret
```

Some networks — many public, campus and guest WiFi setups — block traffic between
clients ("AP isolation"). If `ping` works but the port never opens, that is the likely
cause; a phone hotspot with both machines on it is the quickest way to rule it out.

#### Host failover

A LAN session survives the loss of its host. The host maintains a **roster** — itself
first, then joiners in the order they connected — and rebroadcasts it to everyone on
every membership change. That roster doubles as the succession order.

When the host's socket drops, each survivor independently walks the same list:

1. Drop the dead host from the roster.
2. If the next entry is *me*, promote to host and start serving.
3. Otherwise, dial that survivor's advertised address and rejoin as a joiner,
   retrying briefly while it promotes itself.
4. If that candidate never comes up, skip it and repeat with the next one.

Because every node acts on the same broadcast roster, they converge on the same
successor without any voting round-trip. Failover cascades: kill the promoted host and
the next survivor takes over, down to a lone participant hosting by itself. The
`Active users` line shows the current host and a `[host gen N]` counter that increments
on each promotion.

Every participant binds a listening socket at startup — a joiner's simply sits idle
until it is elected — so promotion needs no new bind and no coordination. A joiner
normally binds the same session port on its own machine; if that port is already taken
locally (two participants on one host, as in the test suite) it falls back to an
ephemeral port, which the host records and publishes in the roster. One consequence:
after a failover in that same-machine case, the new host is reachable at its ephemeral
port rather than the original session port, so a *fresh* `--join` needs that port.

On rejoin, a survivor adopts the new host's snapshot only if it made no local edits
while the link was down; otherwise it keeps its own copy and the normal diff/merge path
reconciles the two.

Limits worth knowing: election is best-effort, not consensus. If the host dies before a
just-connected joiner has received its first roster, two nodes can briefly both promote.
There is no fencing and no split-brain merge, and the document still crosses the wire in
the clear.

LAN example:
```bash
# On the host machine
./build/editor --host notes 123456

# On another machine on the same network
./build/editor --join 192.168.1.25 123456
```

If it's your first time (no binary yet), build then run:
```bash
make editor && ./build/editor notes
```

When first run, your copy `user_docs/<doc>/<you>_doc.txt` is created with the initial content. The program will try to auto-open it for you with this precedence:
- `SYNCTEXT_EDITOR` if set (e.g., `SYNCTEXT_EDITOR=gedit`)
- `$VISUAL`, then `$EDITOR`
- GUI editors (if `$DISPLAY` is set): `gnome-text-editor`, `gedit`, `xed`, `pluma`, `mousepad`, `leafpad`, `kate`, `code`
- Finally `xdg-open`
- Terminal fallback: open `nano` in a new terminal (`x-terminal-emulator`, `gnome-terminal`, or `xterm`)

Set `SYNCTEXT_NO_AUTO_OPEN=1` to disable auto-open entirely.

Notes:
- A shared registry is created at `/synctext_registry` (POSIX shared memory).
- Each participant publishes a POSIX message queue name (e.g., `/synctext_notes_chaitu`) in the registry for that document.
- Up to 5 concurrent users are supported.
- Each user publishes the **absolute** path of its document in the registry. Sessions are
  routinely started from different working directories, and a newly started session has
  to be able to find an existing peer's document to seed itself from.
- Active users list uses heartbeats and process liveness; users are considered inactive if no heartbeat for ~10s or their process has exited.
- LAN mode bypasses POSIX shared memory and message queues. It uses direct TCP connections to the host and still writes one local document per participant under `user_docs/`.
- LAN sessions are not capped at 5 participants; `MAX_USERS` applies only to the shared-memory registry.

Duplicate session handling:
- Identities are derived from your OS account and auto-suffixed (`chaitu`, `chaitu-2`) when one account opens a document twice, so a duplicate is normally resolved without asking. The interactive reclaim prompt only appears when `SYNCTEXT_USER` pinned the identity explicitly; disable it with `SYNCTEXT_INTERACTIVE_LOGIN=0`.
- Non-interactive force reclaim: set `SYNCTEXT_FORCE_RECLAIM=1` to reclaim. The existing process for that user will receive SIGTERM and should close; you can tune the grace period with `SYNCTEXT_RECLAIM_GRACE_MS` (default 1500ms). If it doesn’t exit in time and you also set `SYNCTEXT_FORCE_KILL=1`, it will be SIGKILLed.

Tip: Environment variables must not have spaces around `=`. Example:
```bash
SYNCTEXT_FORCE_RECLAIM=1 ./build/editor notes   # correct
SYNCTEXT_FORCE_RECLAIM = 1 ./build/editor notes # incorrect on bash
```

## Clean up (optional)

To remove build artifacts:
```bash
make clean
```
Runtime objects are not unlinked automatically: the registry that peers use to find each
other stays put, because removing it while another process is registering would split
one session into two that cannot see each other. With no sessions running, clear them
by hand:

```bash
rm -f /dev/shm/synctext_registry     # the default document's registry
rm -f /dev/shm/synctext_reg_*        # one per --doc id
rm -f /dev/mqueue/synctext_*         # per-user message queues
```

Each registry is about 3 KB, so this is housekeeping rather than anything urgent.

## Core features
- Start program with `./build/editor <doc>`
- User registration & discovery via shared memory registry (up to 5 concurrent users)
- Per-participant local copy `user_docs/<doc>/<you>_doc.txt` with initial content
- Continuous monitoring with automatic diff detection
- Real-time terminal display showing current document, active users, and change summaries
- Update objects created for each detected change: type, line, column range, old/new payloads, timestamp, and user id

## Edit operations supported
Inline (same line):
- Insert, Delete, Replace

Structural (line-level):
- LineInsert, LineDelete

Block (multi-line batches):
- BlockInsert, BlockDelete

Application order: structural deletes (descending) → structural inserts (ascending) → inline ops rebased on the structural result. Conflicts resolve with Last-Writer-Wins (nanosecond timestamp; ties broken by user id). Same-user adjacency is allowed (no self-conflict).

Block operations are disabled by default for maximum compatibility and stability. Enable cautiously (see Environment).

## Merge (CRDT-style) overview
- Remote-only arbitration: only remote updates participate in conflict resolution to avoid self-dupes.
- Last-Writer-Wins with user-id tiebreaker.
- Per-line local timestamps drop stale remote updates and prevent “resurrects”.
- Coarse stale-remote suppression: remote inserts older than your last local delete epoch are dropped.
- Deferred-merge write with grace window to avoid racing active editors; post-merge echo suppression window avoids re-diffing our own writes.
- Broadcast chunking and background draining to deliver long sequences without storms.

## Environment

Identity, document and network (these replace what used to be command-line arguments):
- `SYNCTEXT_USER` — pin the identity instead of deriving it from the OS account
- `SYNCTEXT_DOC` — choose a document name from the environment instead of `--doc`
- `SYNCTEXT_PORT_BASE` (default 9000) — first port a host takes / a joiner scans
- `SYNCTEXT_PORT_SPAN` (default 16) — how many consecutive ports to try
- `SYNCTEXT_CONNECT_TIMEOUT_MS` (default 10000) — bound on each connect and handshake
- `SYNCTEXT_MERGE_BATCH_N` (default 1) — merge only after N updates accumulate
- `SYNCTEXT_TUI` (default: on when stdin and stdout are terminals) — built-in editor
- `SYNCTEXT_EDIT_WINDOW` / `SYNCTEXT_EDIT_TTY` — editor in its own window

Stability/latency:
- `SYNCTEXT_DEBOUNCE_MS` (default 300)
- `SYNCTEXT_SETTLE_MS` (default 150)
- `SYNCTEXT_SETTLE_OVERALL_MS` (default 1500)
- `SYNCTEXT_MASS_DIFF_RECHECK_MS` (default 180) — recheck suspicious “mass change” bursts

Broadcasting and queues:
- `SYNCTEXT_BROADCAST_BATCH_N` (default 5)
- `SYNCTEXT_MAX_BROADCAST_PER_CYCLE` (default 128)
- `SYNCTEXT_SEND_RETRY_COUNT` (default 20)
- `SYNCTEXT_SEND_RETRY_DELAY_MS` (default 2)
- `SYNCTEXT_MQ_MAXMSG` (default 256)

Block operations:
- `SYNCTEXT_BLOCK_OPS_MODE` (default 0): 0=never, 1=auto(threshold), 2=always
- `SYNCTEXT_BLOCK_OPS_MIN_LINES` (default 6): minimum lines to trigger block ops in auto

Debugging and status:
- `SYNCTEXT_DEBUG_LEVEL` (0-3; default 0): 0=off, 1=basic, 2=verbose, 3=trace
- `SYNCTEXT_DEBUG_MSG=1` (legacy): maps to `SYNCTEXT_DEBUG_LEVEL=2` if level not set
- `SYNCTEXT_STATUS=1`: print a one-line startup banner of key runtime settings

UI and ergonomics:
- `SYNCTEXT_SPINNER=1` show idle spinner
- `SYNCTEXT_IDLE_REFRESH_SEC` (default 5)
- `SYNCTEXT_PERIODIC_MTIME=1` force periodic mtime checks
- `SYNCTEXT_HOTKEYS=0|1` enable/disable hotkeys (auto-enabled on TTY)
- `SYNCTEXT_NO_AUTO_OPEN=1` disable auto-opening the editor
- `SYNCTEXT_EDITOR` preferred editor (fallback to $VISUAL, $EDITOR, then GUI/CLI list)

Login/session safety:
- `SYNCTEXT_INTERACTIVE_LOGIN=0|1` (default 1 if TTY)
- `SYNCTEXT_FORCE_RECLAIM=1` and `SYNCTEXT_RECLAIM_GRACE_MS` (default 1500)
- `SYNCTEXT_FORCE_KILL=1` if the old process won’t exit

## Platform support

**Linux only.** This is not a build-flag away from portable — three of the interfaces it
is built on are Linux-specific kernel features, not merely POSIX:

| Interface | Used for | Elsewhere |
| --- | --- | --- |
| `inotify` | detecting document changes | Linux only |
| POSIX message queues (`mqueue.h`) | local peer transport | Linux; **not implemented on macOS** |
| `eventfd` | waking the main loop immediately | Linux only |
| `shm_open` | the peer registry | POSIX; no native Windows equivalent |
| `termios` | raw mode for the built-in editor | POSIX |

- **Windows:** no native build. Use **WSL2**, which is a real Linux kernel and runs it
  unchanged.
- **macOS:** does not build as-is. It would need `kqueue`/FSEvents in place of inotify
  and a different local transport, since Apple never implemented POSIX message queues.

### Taking part from Windows (WSL2)

Building and editing work normally inside WSL2. Networking is the part to watch, because
WSL2 sits behind a NAT'd virtual switch:

- **Joining** a host elsewhere on the LAN works — outbound connections leave the VM fine.
- **Hosting** inside WSL2 for other machines does **not** work out of the box: the
  session listens on the VM's address, not the Windows machine's. It needs
  `netsh interface portproxy` forwarding on the Windows side, or Windows 11's mirrored
  networking mode.
- `hostname -I` inside WSL2 prints the VM's `172.x` address, which is **not** the address
  to give other machines.

The simple arrangement: **host on Linux, join from WSL2.**

## Compatibility (important)
All peers should run the same binary, protocol version, and configuration:
- Build the latest once and share the `./build/editor` binary across peers, or rebuild on each machine from the same commit.
- Keep `SYNCTEXT_BLOCK_OPS_MODE` aligned across all users. Default is 0 (disabled) — safest.
- If you enable block ops (1 or 2), ensure every peer also enables them to avoid mismatched operation types.
- Use `SYNCTEXT_STATUS=1` to print a startup banner that helps verify alignment quickly.

## Hassle-free start
You don’t need to type multiple commands every time. From the project root:
```bash
make editor && ./build/editor notes
```
This compiles if needed and starts the editor in one line. To always run with status banner and verbose logs:
```bash
SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 ./build/editor notes
```
