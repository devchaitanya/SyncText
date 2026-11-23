# Code Organization

> **Note:** `src/editor.cpp` has since been split into one file per subsystem, with the code unchanged. The list of files is at the top of `src/main.cpp`; what they share is in `src/shared.h`. Where this document says "editor.cpp", read "the files in `src/`".

This document explains how the SyncText codebase is organized after refactoring for improved readability.

## File Structure

```
SyncText/
├── src/
│   ├── synctext.h      # Header file with declarations
│   └── editor.cpp      # Main implementation
├── Makefile
├── README.md
├── DESIGNDOC.tex       # Detailed design document
├── CODE_STRUCTURE.md   # Detailed code walkthrough
└── run_all_tests.sh    # Comprehensive test suite
```

## Header File: `src/synctext.h`

The header provides a clean interface with:

### Data Structures
- **`UserEntry`** - Single user slot in shared registry (state: 0=empty, 1=reserving, 2=active)
- **`Registry`** - Shared memory registry for user discovery (mmap'd across all processes)
- **`Update`** - Represents a document change (local or remote)
- **`WireHdr`** - Binary message header for update serialization
- **`InotifyState`** - File watch descriptor state
- **`InotifyFlags`** - Parsed inotify event flags
- **`RegisterResult`** - Result of user registration attempt (enum)

### Constants
- `MAX_USERS = 5` - Maximum concurrent users
- `USER_ID_MAX = 32` - Maximum user ID string length
- `QUEUE_NAME_MAX = 64` - Maximum queue name length
- `REGISTRY_SHM_NAME = "/synctext_registry"` - Shared memory name
- `MQ_MSGSIZE = 2048` - Message queue message size
- `RX_RING_CAP = 4096` - Receive ring buffer capacity

### Public API
- `int run_main(int argc, char** argv)` - Main entry point

All implementation details are kept internal to `editor.cpp`.

## Implementation File: `src/editor.cpp`

The implementation is organized into clear sections:

### Section 1: Shared Memory Registry - User Discovery & Coordination
- Struct definitions (moved from inline to using header)
- Global state management
- Registry initialization (`init_registry`)
- Heartbeat mechanism (`heartbeat`)
- User deregistration (`deregister_user`)

### Section 2: Message Queue - Broadcasting Updates
- Queue creation and management (`open_message_queue`)
- Update broadcasting (`broadcast_updates_to_all`)
- Background listener thread (`listener_loop`)

### Section 3: Message Serialization - Binary Protocol for Updates
- Wire format serialization (`serialize_update`)
- Deserialization (`deserialize_update`)
- Binary packing/unpacking

### Section 4: User Registration - Slot Reservation & Discovery
- CAS-based slot reservation (`register_user_try`)
- Force reclaim logic (`attempt_force_reclaim_user`)
- PID liveness checking (`find_active_pid_for_user`, `wait_pid_gone`)

### Section 5: File I/O - Document Read/Write
- File reading with stability checks (`read_file_lines`, `read_file_lines_stable`)
- Initial document creation (`write_initial_doc`)
- Peer document copying (`copy_doc_from_peer_if_possible`)

### Section 6: File Monitoring - Inotify Event Handling
- Inotify initialization (`init_inotify_for_file`)
- Event draining (`drain_inotify_events`)
- Quiescence detection (`settle_inotify_quiet`)

### Section 7: CRDT - Diff Computation & Conflict-Free Merging
- Document diffing (`diff_documents`)
- LWW comparison (`lww_compare`, `a_wins_over_b`)
- Update application (`apply_update_to_lines`)
- Line timestamp management

## Design Principles

### Why This Organization?

1. **Clean Interface**: Header file exposes only essential types and the entry point
2. **Implementation Privacy**: All implementation details remain internal (static)
3. **Logical Grouping**: Seven clear sections make navigation easy
4. **Single Compilation Unit**: No complex build dependencies
5. **Zero Breaking Changes**: All existing functionality preserved

### What We Avoided

This refactoring deliberately **avoided**:
- ❌ Splitting into 10+ separate files (over-engineering)
- ❌ Complex build system changes
- ❌ Exposing internal functions unnecessarily
- ❌ Moving implementation out of editor.cpp

### What We Achieved

- ✅ Clear declaration/definition separation
- ✅ Types visible for better code navigation
- ✅ Well-documented section headers
- ✅ Constants defined once in header
- ✅ All tests still pass (8/8)

## Using the Code

### To Add New Functionality

1. Add struct/enum to `synctext.h` if it needs to be shared
2. Implement in appropriate section of `editor.cpp`
3. Keep functions `static` unless they must be called from `main()`

### To Navigate the Code

1. Start with `synctext.h` to understand data structures
2. Jump to specific section in `editor.cpp`:
   - Registry operations → Section 1
   - Message queue → Section 2
   - Serialization → Section 3
   - Registration → Section 4
   - File I/O → Section 5
   - File monitoring → Section 6
   - CRDT logic → Section 7
3. Use `CODE_STRUCTURE.md` for detailed walkthrough

### To Understand the System

1. **Quick overview**: Read this file (CODE_ORGANIZATION.md)
2. **Architectural details**: Read `DESIGNDOC.tex`
3. **Code walkthrough**: Read `CODE_STRUCTURE.md`
4. **Usage instructions**: Read `README.md`

## Build & Test

After refactoring:

```bash
# Clean build
make clean && make

# Run all tests
bash run_all_tests.sh
```

**Result**: ✅ Clean compilation, all 8 tests passing

## Summary

The code is now organized with:
- A clean header file for types and constants
- All implementation in a single, well-sectioned file
- Clear separation between public API and internal details
- No loss of functionality or performance

This organization prioritizes **readability and maintainability** without the complexity of a multi-file architecture.
