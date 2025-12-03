# SyncText Test Suite

This folder contains all test scripts for the SyncText collaborative editor.

## Running Tests

All tests should be run from the project root directory:

```bash
cd /home/chaitu/Desktop/vscode/SyncText
./tests/<test_name>.sh
```

## Test Files

### Core Functionality Tests
- **conflict_test.sh** - Complete test suite for all 4 conflict scenarios (non-conflicting, LWW, insert, delete)
- **test_paste_no_debug.sh** - Tests 13-line paste propagation (original bug report)

### Mass Operation Tests
- **test_insert_many.sh** - Tests 20-line insert
- **test_30lines.sh** - Tests 30-line insert (verifies "every 9th line" fix)
- **test_50lines.sh** - Tests 50-line insert (stress test for queue flow control)
- **test_line21.sh** - Focused test for line 21 issue with debug logging
- **test_delete_all.sh** - Tests deleting all lines with content-based recovery

### Debug/Development Tests
- **test_line9_issue.sh** - Tests 10-line insert (original Line 9 investigation)
- **test_debug_inserts.sh** - Debug helper for insert operations
- **test_multiline_ops.sh** - Multi-line operation testing
- **test_multiline_paste.sh** - Multi-line paste scenarios

### Legacy/Scratch Tests
- **debug_scenario1.sh** - Scenario 1 isolation test
- **test_scenario1.sh** - Standalone Scenario 1 test
- **test_long_wait.sh** - Long wait timing test
- **edits.sh** - General edit testing
- **test.sh** - Miscellaneous test

## Test Results (Latest)

All core tests passing with queue flow control fix (retry_count=200, retry_delay=5ms):

- ✅ 13-line paste: PASS
- ✅ 20-line insert: PASS  
- ✅ 30-line insert: PASS (all lines including 11, 23)
- ✅ 50-line insert: PASS
- ✅ All 4 conflict scenarios: PASS (4/4)
- ✅ Delete-all operation: PASS

## Key Issues Fixed

1. **Multi-line paste bug** - Lines 9-13 missing → Fixed by correcting `g_last_local_change_ns` tracking
2. **Timestamp precision** - Same-second changes missed → Fixed with nanosecond-precision timestamps
3. **Mass operation failures** - "Every 9th line" failing → Fixed by increasing queue retry parameters
4. **Delete-all crashes** - Out-of-bounds line deletion → Fixed with content-based fallback

## Environment Variables

Tests can be customized with these environment variables:
- `SYNCTEXT_DEBUG_LEVEL` - Debug verbosity (0-3)
- `SYNCTEXT_SEND_RETRY_COUNT` - Number of retries on queue full (default: 200)
- `SYNCTEXT_SEND_RETRY_DELAY_MS` - Delay between retries in ms (default: 5)
