/**
 * @file CODE_STRUCTURE.md
 * @brief Comprehensive guide to SyncText code organization
 * 
 * This document provides a roadmap through editor.cpp's 2100+ lines,
 * explaining the purpose of each major section and key functions.
 */

# SyncText Code Structure Guide

## File Organization Overview

The main source file `src/editor.cpp` is organized into the following logical sections:

### 1. HEADER & CONFIGURATION (Lines 1-130)
- File-level documentation explaining CRDT, batching, inotify
- Standard library includes
- POSIX API includes (mqueue, shm, inotify)
- Registry data structures (UserEntry, Registry)
- Global configuration variables
- Function forward declarations

### 2. SHARED MEMORY REGISTRY (Lines 131-280)
**Purpose**: User discovery and coordination via lock-free shared memory

**Key Functions**:
- `init_registry()` - Create or attach to /synctext_registry
- `register_user_try()` - Atomic CAS-based slot reservation
- `heartbeat()` - Update liveness timestamp
- `deregister_user()` - Release registry slot
- `find_active_pid_for_user()` - Check if user is active
- `attempt_force_reclaim_user()` - SIGTERM/SIGKILL old process

**Data Structures**:
- `UserEntry`: Per-user slot with atomic state machine
- `Registry`: Array of MAX_USERS entries in shared memory

### 3. MESSAGE QUEUE OPERATIONS (Lines 281-450)
**Purpose**: POSIX message queue setup and update transmission

**Key Functions**:
- `make_queue_name()` - Generate /synctext_user_<id> name
- `open_message_queue()` - Create receive queue with custom attributes
- `serialize_update()` - Pack Update into wire format
- `deserialize_update()` - Unpack Update from wire format
- `broadcast_updates_to_all()` - Send updates to all active peers

**Wire Protocol**:
- Fixed WireHdr (32 bytes) + variable-length strings
- Retry logic: 200 attempts × 5ms delay for EAGAIN resilience

### 4. UPDATE DATA STRUCTURE (Lines 451-580)
**Purpose**: Represent document changes for propagation and merge

**Key Types**:
- `Update::Type`: Insert, Delete, Replace, LineInsert, LineDelete, BlockInsert, BlockDelete
- `WireHdr`: Binary serialization header

**Key Functions**:
- `type_to_string()` - Convert enum to human-readable string
- `block_line_count()` - Count lines in block operations

### 5. RING BUFFER & LISTENER THREAD (Lines 581-700)
**Purpose**: Lock-free message reception via background thread

**Key Components**:
- `g_rx_ring[]` - Fixed-size SPSC queue (4096 capacity)
- `g_rx_head`/`g_rx_tail` - Atomic producer/consumer indices
- `listener_loop()` - Background thread draining message queue
- `rx_ring_push()`/`rx_ring_pop()` - Lock-free enqueue/dequeue

**Flow**:
1. Listener thread calls mq_receive() in loop
2. Deserializes updates and pushes to ring buffer
3. Wakes main loop via eventfd
4. Main loop pops from ring into g_rx_local vector

### 6. FILE I/O & CHANGE DETECTION (Lines 701-880)
**Purpose**: Robust file reading with transient-state handling

**Key Functions**:
- `read_file_lines()` - Basic line-by-line read
- `read_file_lines_stable()` - Retry until size stabilizes
- `timespec_equal()` - Compare nanosecond mtime
- `write_initial_doc()` - Create default document
- `copy_doc_from_peer_if_possible()` - Bootstrap from active user

**Robustness**:
- Detects transient empty snapshots (editor truncate+rewrite)
- Debounce + settle windows prevent partial reads
- Suspicious ratio check triggers reread

### 7. DIFF ALGORITHM (Lines 881-1100)
**Purpose**: Compute line-level and character-level differences

**Key Functions**:
- `diff_documents()` - Main diff entry point
- `detect_line_change()` - Classify line modifications
- `allow_block_op()` - Heuristic for batch operations

**Diff Types**:
- **Structural**: LineInsert (new line), LineDelete (removed line)
- **Inline**: Insert/Delete/Replace (within-line changes)
- **Block**: BlockInsert/BlockDelete (multi-line batches)

**Algorithm**:
1. Align old and new line arrays
2. Detect line insertions and deletions
3. For matching lines, compute character-level diff
4. Batch adjacent operations if block ops enabled

### 8. CRDT MERGE ENGINE (Lines 1101-1400)
**Purpose**: Conflict resolution via Last-Writer-Wins (LWW) with structural-first ordering

**Key Functions**:
- `lww_compare()` - Compare timestamps, tie-break by user_id
- `a_wins_over_b()` - Boolean LWW predicate
- `ranges_overlap()` - Detect spatial conflicts
- `apply_update_to_lines()` - Execute update on document
- `is_structural()` - Check if update modifies line structure
- `update_local_line_timestamps_after()` - Track local modifications

**Merge Strategy**:
1. Partition updates: structural deletes, structural inserts, inline ops
2. Sort structural deletes descending (stable indices)
3. Sort structural inserts ascending
4. Apply deletes → inserts → inline (rebased on new structure)
5. Resolve conflicts by timestamp (nanosecond precision)
6. Suppress stale updates (older than local line timestamp)
7. Write merged result after grace period

**Batch Merging**:
- Threshold N (default 1) controls merge frequency
- N=1: Immediate merge (interactive editing)
- N>1: Accumulate N updates before merge (bulk operations)

### 9. UI RENDERING (Lines 1401-1550)
**Purpose**: Terminal display with color-coded change indicators

**Key Functions**:
- `clear_screen()` - ANSI clear sequence
- `stdout_supports_color()` - Check for TTY and $TERM
- `col_green()`/`col_red()`/`col_yellow()`/`col_cyan()` - ANSI color codes
- `render_document()` - Print document with change markers

**Display Elements**:
- Document path and last modified time
- Line-by-line content with [MODIFIED] markers
- Active users list (from registry)
- Spinner/periodic status
- Hotkey legend (q/o/s/p)

### 10. INOTIFY INTEGRATION (Lines 1551-1680)
**Purpose**: Efficient file change detection via Linux inotify

**Key Functions**:
- `init_inotify_for_file()` - Create watch descriptor
- `drain_inotify_events()` - Read and classify events
- `settle_inotify_quiet()` - Wait for event quiescence

**Events Handled**:
- `IN_MODIFY` - Content changed
- `IN_ATTRIB` - Metadata changed (mtime)
- `IN_CLOSE_WRITE` - File closed after write
- `IN_MOVE_SELF` - File replaced via rename
- `IN_DELETE_SELF` - File deleted

**Fallback**:
- If inotify unavailable: 1-second polling via mtime comparison
- If watch fails after MOVE_SELF: re-establish or fall back to polling

### 11. EXTERNAL EDITOR INTEGRATION (Lines 1681-1800)
**Purpose**: Auto-launch user's preferred editor

**Key Functions**:
- `spawn_bg_argv()` - Fork/exec with posix_spawn
- `spawn_bg_shell()` - Execute shell command
- `open_local_editor_if_possible()` - Editor selection heuristic

**Editor Selection**:
1. $SYNCTEXT_EDITOR (explicit override)
2. $VISUAL or $EDITOR (standard UNIX)
3. GUI editors if $DISPLAY set (gnome-text-editor, gedit, kate, VS Code)
4. Terminal editors in new terminal (nano via x-terminal-emulator)

### 12. TERMINAL INTERACTION (Lines 1801-1900)
**Purpose**: Non-blocking hotkey processing

**Key Functions**:
- `set_terminal_rawmode()` - Disable canonical mode
- `restore_terminal_mode()` - Restore original termios
- `handle_keypresses_nonblock()` - Poll stdin for commands

**Hotkeys**:
- `q` - Quit program
- `o` - Open document in external editor
- `s` - Toggle spinner animation
- `p` - Toggle periodic updates

### 13. CONFIGURATION PARSING (Lines 1901-2000)
**Purpose**: Read environment variables for runtime tuning

**Key Functions**:
- `parse_env_bool()` - Boolean variables
- `parse_env_seconds()` - Time in seconds
- `parse_env_millis()` - Time in milliseconds

**Environment Variables**:
- `SYNCTEXT_MERGE_BATCH_N` - Merge threshold
- `SYNCTEXT_DEBOUNCE_MS` - Debounce delay
- `SYNCTEXT_SEND_RETRY_COUNT` - Queue retry count
- `SYNCTEXT_SEND_RETRY_DELAY_MS` - Retry delay
- Many more (see README.md Configuration section)

### 14. MAIN EVENT LOOP (Lines 2001-2090)
**Purpose**: Orchestrate all subsystems

**Flow**:
1. Parse command-line arguments (user_id, optional merge_batch_n)
2. Initialize registry and register user
3. Create directories (user_docs/, log_files/)
4. Ensure local document exists
5. Open message queue and start listener thread
6. Enter main loop:
   a. Check for inotify events or poll mtime
   b. If changed: debounce, settle, read stable snapshot
   c. Compute diff vs. previous snapshot
   d. Broadcast local updates to all peers
   e. Drain received updates from ring buffer
   f. Trigger merge if batch threshold reached
   g. Apply remote updates via CRDT merge
   h. Write merged result after grace period
   i. Render UI with latest state
   j. Heartbeat every ~1 second
   k. Handle hotkeys if enabled
7. Cleanup: deregister user, close queues, restore terminal

## Key Algorithms

### Atomic Slot Reservation (Registry)
```cpp
// Try to reserve empty slot using CAS
for (each slot) {
    uint32_t expected = 0;
    if (slot.active.compare_exchange_strong(expected, 1)) {
        // Reserved! Fill in details
        slot.pid = getpid();
        strcpy(slot.user_id, user_id);
        strcpy(slot.queue_name, queue_name);
        slot.active.store(2); // Commit to active
        return Ok;
    }
}
return NoSlots;
```

### LWW Conflict Resolution
```cpp
int lww_compare(Update a, Update b) {
    if (a.ts_ns != b.ts_ns)
        return (a.ts_ns > b.ts_ns) ? 1 : -1;
    // Tie-break by user_id lexicographic order
    return a.user_id.compare(b.user_id);
}
```

### Structural-First Merge
```cpp
// Partition updates
vector<Update> deletes, inserts, inline;
for (update : remote_updates) {
    if (update.type == LineDelete) deletes.push(update);
    else if (update.type == LineInsert) inserts.push(update);
    else inline.push(update);
}

// Apply in order to maintain index stability
sort(deletes, descending(line_no));
sort(inserts, ascending(line_no));

for (del : deletes) apply(del);
for (ins : inserts) apply(ins);

// Rebase inline ops on new line structure
for (op : inline) {
    adjust_line_index(op);
    apply(op);
}
```

### Retry with Backoff (Message Queue)
```cpp
for (int attempt = 0; attempt < g_send_retry_count; ++attempt) {
    int ret = mq_send(mq, msg, len, 0);
    if (ret == 0) return true; // Success
    
    if (errno == EAGAIN) {
        // Queue full, wait and retry
        usleep(g_send_retry_delay_ms * 1000);
        continue;
    }
    
    // Other error, give up
    return false;
}
return false; // Exhausted retries
```

## Data Flow Diagram

```
[External Editor]
       |
       v (save file)
[inotify / mtime poll] ────> [Debounce + Settle]
       |
       v (file changed)
[read_file_lines_stable()] ──> [diff_documents()]
       |
       v (Update objects)
[broadcast_updates_to_all()] ─┬─> [Peer 1 mqueue]
                              ├─> [Peer 2 mqueue]
                              └─> [Peer 3 mqueue]

[mqueue receive] <───────────────[Remote Peer]
       |
       v (binary message)
[deserialize_update()] ──> [rx_ring_push()]
       |
       v (eventfd wake)
[rx_ring_pop()] ──> [g_rx_local accumulation]
       |
       v (batch threshold reached)
[CRDT merge: LWW + structural-first] ──> [Merged lines]
       |
       v (after grace period)
[write to disk] ──> [Echo suppression]
       |
       v
[render_document()] ──> [Terminal UI]
```

## Threading Model

- **Main Thread**: Event loop, file monitoring, UI, merge application
- **Listener Thread**: Background mq_receive() loop, ring buffer producer
- **No locks**: Communication via lock-free ring buffer + atomic flags
- **Synchronization**: eventfd for wake notifications

## Critical Paths (Performance-Sensitive)

1. **Change Detection**: inotify + debounce (~80ms latency)
2. **Broadcasting**: mq_send with retry (1-10ms typical, up to 1s backpressure)
3. **Ring Buffer**: Lock-free push/pop (nanoseconds)
4. **CRDT Merge**: O(n*m) for n remote updates and m document lines
5. **UI Render**: O(n) for n lines, skipped if no changes

## Error Handling Strategies

- **Registry Conflicts**: CAS retry loop (spin briefly)
- **Message Queue Full**: Exponential backoff with bounded retries
- **File Transient States**: Debounce + settle + recheck
- **Stale Updates**: Timestamp filtering at merge time
- **Process Crashes**: Heartbeat expiry + slot reclaim

## Testing Coverage

Each major subsystem has dedicated tests:
- **Multi-line paste**: Nanosecond timestamp precision
- **Mass insert (50 lines)**: Queue flow control
- **Delete-all**: Structural operation handling
- **Conflict scenarios**: LWW resolution correctness
- **Merge batching**: Threshold behavior (N=1 vs N=5)

See `run_all_tests.sh` for comprehensive test suite.

## Extension Points

To add new features:
1. **New update types**: Extend Update::Type enum
2. **Custom diff**: Modify diff_documents() heuristics
3. **Alternative merge**: Replace lww_compare() logic
4. **Network transport**: Replace mqueue with sockets
5. **Encryption**: Add crypto layer in serialize/deserialize

## Performance Tuning

Key environment variables for different workloads:

**Interactive Editing** (low latency):
```bash
export SYNCTEXT_MERGE_BATCH_N=1
export SYNCTEXT_DEBOUNCE_MS=50
./build/editor user1
```

**Bulk Operations** (high throughput):
```bash
export SYNCTEXT_MERGE_BATCH_N=20
export SYNCTEXT_SEND_RETRY_COUNT=500
./build/editor user1 20
```

**Debug/Development** (verbose logging):
```bash
export SYNCTEXT_DEBUG_LEVEL=2
./build/editor user1
```

## Common Gotchas

1. **Timestamp precision**: Must use nanoseconds, not seconds
2. **Index stability**: Apply deletes descending, inserts ascending
3. **Transient files**: Always debounce + settle before reading
4. **Queue sizing**: Ensure MQ_MAXMSG ≥ max burst size
5. **Echo suppression**: Don't re-diff immediately after merge write
6. **Heartbeats**: Fail to send heartbeat → appear stale → slot reclaimed

## Additional Resources

- **README.md**: User-facing documentation (build, run, test)
- **DESIGNDOC.tex**: Academic-style design rationale (LaTeX)
- **MERGE_BATCH_SIZE.md**: Detailed merge batching guide
- **run_all_tests.sh**: Comprehensive test suite with explanations
