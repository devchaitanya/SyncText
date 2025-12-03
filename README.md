# SyncText — CRDT-Based Collaborative Text Editor

SyncText is a lightweight collaborative editor that discovers peers via a shared-memory registry and exchanges edits over POSIX message queues. It maintains convergence using a pragmatic CRDT-style merge with Last-Writer-Wins arbitration.

## Build

Requirements (Linux):
- g++ (C++17)
- POSIX APIs available (shm, mq, stat)

```bash
make
```

## Quick Test

Run the comprehensive test suite:
```bash
./run_all_tests.sh
```
This runs all 8 major tests:
- Multi-line paste (13 lines)
- Mass insert (50 lines)
- Delete-all operation
- Non-conflicting edits
- Conflicting edits (LWW)
- Structural line insert
- Structural line delete

Individual tests are in the `tests/` directory.

## Run

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

Quick start (no hassle):
```bash
./build/editor user1
```

### Command-line Arguments

The editor can be launched with the following arguments:

```bash
./build/editor <user_id> [merge_batch_n]
```

-   **`<user_id>`** (required): A unique identifier for the user session.
-   **`[merge_batch_n]`** (optional): The number of incoming updates to accumulate before applying them as a single batch.
    -   **Default**: `1` (immediate merge).
    -   **Range**: `1` to `1000`.
    -   **Example**: `./build/editor user1 5` will wait for 5 updates before merging. This can improve performance during high-volume edits by reducing the frequency of file writes.

If it's your first time (no binary yet), build then run:
```bash
make editor && ./build/editor user1
```

When first run, your local file `<user_id>_doc.txt` is created with the initial content. The program will try to auto-open it for you with this precedence:
- `SYNCTEXT_EDITOR` if set (e.g., `SYNCTEXT_EDITOR=gedit`)
- `$VISUAL`, then `$EDITOR`
- GUI editors (if `$DISPLAY` is set): `gnome-text-editor`, `gedit`, `xed`, `pluma`, `mousepad`, `leafpad`, `kate`, `code`
- Finally `xdg-open`
- Terminal fallback: open `nano` in a new terminal (`x-terminal-emulator`, `gnome-terminal`, or `xterm`)

Set `SYNCTEXT_NO_AUTO_OPEN=1` to disable auto-open entirely.

Notes:
- A shared registry is created at `/synctext_registry` (POSIX shared memory).
- Each user publishes a POSIX message queue name (e.g., `/synctext_user_user1`) in the registry.
- Up to 5 concurrent users are supported.
- Active users list uses heartbeats and process liveness; users are considered inactive if no heartbeat for ~10s or their process has exited.

Duplicate session handling:
- If you start with an already-active `<user_id>` in a terminal, an interactive prompt is shown by default: "Press f to force reclaim, q to quit, any other key to retry". Disable with `SYNCTEXT_INTERACTIVE_LOGIN=0`.
- Non-interactive force reclaim: set `SYNCTEXT_FORCE_RECLAIM=1` to reclaim. The existing process for that user will receive SIGTERM and should close; you can tune the grace period with `SYNCTEXT_RECLAIM_GRACE_MS` (default 1500ms). If it doesn’t exit in time and you also set `SYNCTEXT_FORCE_KILL=1`, it will be SIGKILLed.

Tip: Environment variables must not have spaces around `=`. Example:
```bash
SYNCTEXT_FORCE_RECLAIM=1 ./build/editor user1   # correct
SYNCTEXT_FORCE_RECLAIM = 1 ./build/editor user1 # incorrect on bash
```

## Clean up (optional)

To remove build artifacts:
```bash
make clean
```
The shared memory object `/synctext_registry` is not unlinked automatically unless all users exit and the last process decides to cleanup. You can manually remove it if necessary:
```bash
# Optional manual cleanup (requires no active users)
# sudo rm /dev/shm/synctext_registry  # Typically where shm objects are backed
```

## Core features
- Start program with `./build/editor <user_id> [merge_batch_n]`
- User registration & discovery via shared memory registry (up to 5 concurrent users)
- Per-user local document `<user_id>_doc.txt` with initial content
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

## Compatibility (important)
All peers should run the same binary, protocol version, and configuration:
- Build the latest once and share the `./build/editor` binary across peers, or rebuild on each machine from the same commit.
- Keep `SYNCTEXT_BLOCK_OPS_MODE` aligned across all users. Default is 0 (disabled) — safest.
- If you enable block ops (1 or 2), ensure every peer also enables them to avoid mismatched operation types.
- Use `SYNCTEXT_STATUS=1` to print a startup banner that helps verify alignment quickly.

## Hassle-free start
You don’t need to type multiple commands every time. From the project root:
```bash
make editor && ./build/editor user1
```
This compiles if needed and starts the editor in one line. To always run with status banner and verbose logs:
```bash
SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 ./build/editor user1
```
