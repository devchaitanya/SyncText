#!/usr/bin/env bash
# Comprehensive test harness for SyncText scenarios:
# - Scenario 1: Non-conflicting edits (line 0 and line 1 modified by different users)
# - Scenario 2: Conflicting edits (same line, LWW winner)
# - Edge A: Structural line insert (between lines)
# - Edge B: Structural line delete
#
# Usage: ./conflict_test.sh [userA userB]
# Env knobs:
#   BUILD=1                  Rebuild before running (default 1)
#   STATUS=1                 Show status banner in editors
#   DEBUG_LEVEL=1            Debug verbosity in editors
#   CLEAN=1                  Kill existing editors for userA/userB before start
#   WIN_DELAY_MS=300         Delay between first and second conflicting write
#   FINAL_WAIT_MS=1000       Wait after writes for convergence
#   MERGE_GRACE_MS=300       Merge write grace window (propagated to editors)
set -euo pipefail

USER_A=${1:-u1}
USER_B=${2:-u2}
BUILD=${BUILD:-1}
STATUS=${STATUS:-1}
DEBUG_LEVEL=${DEBUG_LEVEL:-1}
CLEAN=${CLEAN:-1}
WIN_DELAY_MS=${WIN_DELAY_MS:-300}
FINAL_WAIT_MS=${FINAL_WAIT_MS:-1500}
MERGE_GRACE_MS=${MERGE_GRACE_MS:-300}

BIN=./build/editor

say() { echo -e "$*"; }

if [ "$BUILD" = "1" ]; then
  say "[test] Building..."
  make -j >/dev/null
fi

kill_editors() {
  local a b
  a=$(pgrep -af "${BIN} ${USER_A}" | awk '{print $1}' | head -1 || true)
  b=$(pgrep -af "${BIN} ${USER_B}" | awk '{print $1}' | head -1 || true)
  if [ -n "$a" ]; then kill "$a" || true; fi
  if [ -n "$b" ]; then kill "$b" || true; fi
}

cleanup_ipc() {
  # Remove IPC resources for clean state between scenarios
  rm -f "/dev/mqueue/synctext_user_${USER_A}" 2>/dev/null || true
  rm -f "/dev/mqueue/synctext_user_${USER_B}" 2>/dev/null || true
  rm -f /dev/shm/synctext_registry 2>/dev/null || true
}

launch_editor() {
  local u="$1"
  # Always kill and relaunch for clean per-scenario state
  local pid=$(pgrep -af "${BIN} ${u}" | awk '{print $1}' | head -1 || true)
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    sleep 0.2
  fi
  SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=$STATUS SYNCTEXT_DEBUG_LEVEL=$DEBUG_LEVEL \
  SYNCTEXT_MERGE_WRITE_GRACE_MS=$MERGE_GRACE_MS SYNCTEXT_STARTUP_SYNC=${SYNCTEXT_STARTUP_SYNC:-1} \
    "$BIN" "$u" >"log_files/conflict_${u}.log" 2>&1 &
  say "[test] Launched editor for $u (pid $!)"
  sleep 0.4
}

wait_ms() {
  local ms=${1:-0}
  if [ "$ms" -gt 0 ]; then python3 - "$ms" <<'PY'
import sys,time
ms=int(sys.argv[1]); time.sleep(ms/1000.0)
PY
  fi
}

line_equals() {
  # usage: line_equals file line_no expected
  local f="$1"; local ln="$2"; local exp="$3"
  local got
  got=$(sed -n "${ln}p" "$f" 2>/dev/null || true)
  if [ "$got" != "$exp" ]; then
    say "[FAIL] $f line $ln mismatch: got='$got' expected='$exp'"; return 1
  fi
  return 0
}

print_file() { say "----- $1 -----"; nl -ba "$1"; say "----------------"; }

reset_base() {
  # usage: reset_base file x y z
  local f="$1"; local xv="$2"; local yv="$3"; local zv="$4"
  printf "int x = %s;\nint y = %s;\nint z = %s;\n" "$xv" "$yv" "$zv" >"$f"
}

prepare_baseline() {
  # usage: prepare_baseline x y z
  local xv="$1" yv="$2" zv="$3"
  # Remove old files to ensure clean baseline
  rm -f "$FILE_A" "$FILE_B"
  reset_base "$FILE_A" "$xv" "$yv" "$zv"
  reset_base "$FILE_B" "$xv" "$yv" "$zv"
}

start_fresh() {
  if [ "$CLEAN" = "1" ]; then kill_editors; sleep 0.5; fi
  # Launch editors with startup sync disabled to use only the baseline files we prepared
  SYNCTEXT_STARTUP_SYNC=0 launch_editor "$USER_A"
  SYNCTEXT_STARTUP_SYNC=0 launch_editor "$USER_B"
  # Give editors a moment to watch and digest baseline
  sleep 0.8
}

FILE_A="user_docs/${USER_A}_doc.txt"
FILE_B="user_docs/${USER_B}_doc.txt"

PASS_SUM=0; FAIL_SUM=0
pass() { say "[PASS] $1"; PASS_SUM=$((PASS_SUM+1)); }
fail() { say "[FAIL] $1"; FAIL_SUM=$((FAIL_SUM+1)); }

scenario_non_conflicting() {
  say "\n=== Scenario 1: Non-conflicting edits ==="
  # Force kill any lingering editors and clean IPC resources
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.5
  cleanup_ipc
  sleep 0.3
  # Base: 10/20/30 to match description (write BEFORE editors start)
  prepare_baseline 10 20 30
  say "[test] Baseline files prepared: x=10 y=20 z=30"
  start_fresh
  # u1 changes line 1 (1-based) => our file line 1 is 'int x = 10;' -> to '50'
  sed -i '1s@.*@int x = 50;@' "$FILE_A"
  wait_ms 300
  # u2 changes line 2 (int y)
  sed -i '2s@.*@int y = 100;@' "$FILE_B"
  wait_ms "$FINAL_WAIT_MS"
  wait_ms 2000  # Additional 2-second wait before checking
  local ok=1
  for f in "$FILE_A" "$FILE_B"; do
    line_equals "$f" 1 'int x = 50;' || ok=0
    line_equals "$f" 2 'int y = 100;' || ok=0
    line_equals "$f" 3 'int z = 30;' || ok=0
    print_file "$f"
  done
  if [ $ok -eq 1 ]; then pass "Scenario 1 merged successfully"; else fail "Scenario 1 failed"; fi
  # Kill editors after scenario
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.3
}

scenario_conflicting_lww() {
  say "\n=== Scenario 2: Conflicting edits (LWW) ==="
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.5
  cleanup_ipc
  sleep 0.3
  prepare_baseline 42 100 30
  start_fresh
  say "[conflict] First edit by $USER_A (x=75)"
  sed -i '1s@.*@int x = 75;@' "$FILE_A"
  wait_ms "$WIN_DELAY_MS"
  say "[conflict] Second edit by $USER_B (x=99)"
  sed -i '1s@.*@int x = 99;@' "$FILE_B"
  wait_ms "$FINAL_WAIT_MS"
  wait_ms 2000  # Additional 2-second wait before checking
  local ok=1
  for f in "$FILE_A" "$FILE_B"; do
    line_equals "$f" 1 'int x = 99;' || ok=0
    line_equals "$f" 2 'int y = 100;' || ok=0
    line_equals "$f" 3 'int z = 30;' || ok=0
    print_file "$f"
  done
  if [ $ok -eq 1 ]; then pass "Scenario 2 LWW winner propagated"; else fail "Scenario 2 failed"; fi
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.3
}

edge_insert_line() {
  say "\n=== Edge A: Structural line insert ==="
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.5
  cleanup_ipc
  sleep 0.3
  prepare_baseline 1 2 3
  start_fresh
  # Insert new line between 1 and 2 at position 2 (1-based)
  # Create a temp with inserted line for USER_A
  awk 'NR==1{print; print "int mid = 77;"; next} {print}' "$FILE_A" > "$FILE_A.tmp" && mv "$FILE_A.tmp" "$FILE_A"
  wait_ms "$FINAL_WAIT_MS"
  wait_ms 2000  # Additional 2-second wait before checking
  local ok=1
  for f in "$FILE_A" "$FILE_B"; do
    line_equals "$f" 1 'int x = 1;' || ok=0
    line_equals "$f" 2 'int mid = 77;' || ok=0
    line_equals "$f" 3 'int y = 2;' || ok=0
    line_equals "$f" 4 'int z = 3;' || ok=0
    print_file "$f"
  done
  if [ $ok -eq 1 ]; then pass "Edge A insert propagated"; else fail "Edge A failed"; fi
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.3
}

edge_delete_line() {
  say "\n=== Edge B: Structural line delete ==="
  pkill -9 -f "${BIN}" 2>/dev/null || true
  sleep 0.5
  cleanup_ipc
  sleep 0.3
  prepare_baseline 11 22 33
  start_fresh
  # USER_B deletes line 2 (int y)
  sed -i '2d' "$FILE_B"
  wait_ms "$FINAL_WAIT_MS"
  wait_ms 2000  # Additional 2-second wait before checking
  local ok=1
  for f in "$FILE_A" "$FILE_B"; do
    line_equals "$f" 1 'int x = 11;' || ok=0
    line_equals "$f" 2 'int z = 33;' || ok=0
    # Ensure only two lines remain
    local count; count=$(wc -l < "$f")
    if [ "$count" -ne 2 ]; then say "[FAIL] $f expected 2 lines, got $count"; ok=0; fi
    print_file "$f"
  done
  if [ $ok -eq 1 ]; then pass "Edge B delete propagated"; else fail "Edge B failed"; fi
  pkill -9 -f "${BIN}" 2>/dev/null || true
}

# Run all scenarios
scenario_non_conflicting
scenario_conflicting_lww
edge_insert_line
edge_delete_line

say "\n=== Summary ==="
say "PASS: $PASS_SUM  FAIL: $FAIL_SUM"
if [ "$FAIL_SUM" -gt 0 ]; then exit 1; fi
exit 0
