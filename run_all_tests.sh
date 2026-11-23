#!/bin/bash
# SyncText - Comprehensive Test Suite
# Runs all major tests in sequence and reports results

set -e

BIN="./build/editor"
DOC="suite"            # the suite works on one named document
DDIR="user_docs/$DOC"  # where its per-participant copies live
# Never open real terminal windows while testing; the editor-window test names a pty
# explicitly via SYNCTEXT_EDIT_TTY, which takes precedence over this.
export SYNCTEXT_EDIT_WINDOW=0
PASS_TOTAL=0
FAIL_TOTAL=0

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "╔════════════════════════════════════════════════════════════╗"
echo "║          SYNCTEXT COMPREHENSIVE TEST SUITE                 ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Cleanup function
cleanup() {
    pkill -9 -f "$BIN" 2>/dev/null || true
    # Scoped sessions (--doc) create their own registry and queues, so clear those too
    # or leftovers from one test can be discovered by the next.
    rm -f /dev/mqueue/synctext_* /dev/shm/synctext_registry /dev/shm/synctext_reg_* 2>/dev/null || true
    rm -rf "$DDIR" 2>/dev/null || true
    mkdir -p "$DDIR" 2>/dev/null || true
    sleep 0.5
}

# Test result tracking
pass_test() {
    echo -e "${GREEN}[PASS]${NC} $1"
    PASS_TOTAL=$((PASS_TOTAL + 1))
}

fail_test() {
    echo -e "${RED}[FAIL]${NC} $1"
    FAIL_TOTAL=$((FAIL_TOTAL + 1))
}

section() {
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${YELLOW}$1${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
}

# Build
section "Building SyncText"
if make -j >/dev/null 2>&1; then
    pass_test "Build successful"
else
    fail_test "Build failed"
    exit 1
fi

# ============================================================================
# TEST 1: Multi-line Paste (13 lines)
# ============================================================================
section "Test 1: Multi-line Paste (13 lines)"
cleanup

echo "int x = 10;" > $DDIR/u1_doc.txt
echo "int x = 10;" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

cat > $DDIR/u1_doc.txt << 'EOF'
Scenario 1: Non-Conflicting Edits 
User_1 edits their file: Opens user_1_doc.txt in Notepad, changes line 0, saves: 
Line 0: int x = 50;  (changed 10 to 50) 
 
Terminal 1 updates: 
Document: user_1_doc.txt 
Last updated: 14:05:10 
---------------------------------------- 
Line 0: int x = 50;  [MODIFIED] 
Line 1: int y = 20; 
Line 2: int z = 30; 
---------------------------------------- 
Change detected: Line 0, col 8
EOF

sleep 3

U1_LINES=$(wc -l < $DDIR/u1_doc.txt)
U2_LINES=$(wc -l < $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq "$U2_LINES" ] && [ "$U1_LINES" -eq 13 ]; then
    U1_LINE9=$(sed -n '9p' $DDIR/u1_doc.txt)
    U2_LINE9=$(sed -n '9p' $DDIR/u2_doc.txt)
    if [ "$U1_LINE9" = "$U2_LINE9" ]; then
        pass_test "13-line paste propagated correctly"
    else
        fail_test "Line 9 differs between u1 and u2"
    fi
else
    fail_test "Line count mismatch (u1: $U1_LINES, u2: $U2_LINES, expected: 13)"
fi

# ============================================================================
# TEST 2: Mass Insert (50 lines)
# ============================================================================
section "Test 2: Mass Insert (50 lines)"
cleanup

echo "Line 0" > $DDIR/u1_doc.txt
echo "Line 0" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

{
  echo "Line 0"
  for i in $(seq 1 50); do
    echo "Line $i"
  done
} > $DDIR/u1_doc.txt

sleep 3

U1_LINES=$(wc -l < $DDIR/u1_doc.txt)
U2_LINES=$(wc -l < $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq "$U2_LINES" ] && [ "$U1_LINES" -eq 51 ]; then
    # Check for specific lines that previously failed
    MISSING=0
    for line_num in 11 23; do
        if ! grep -q "^Line $line_num$" $DDIR/u2_doc.txt; then
            MISSING=$((MISSING + 1))
        fi
    done
    
    if [ $MISSING -eq 0 ]; then
        pass_test "50-line insert propagated correctly"
    else
        fail_test "$MISSING lines missing (lines 11, 23)"
    fi
else
    fail_test "Line count mismatch (u1: $U1_LINES, u2: $U2_LINES, expected: 51)"
fi

# ============================================================================
# TEST 3: Delete All Lines
# ============================================================================
section "Test 3: Delete All Lines"
cleanup

{
  echo "Line 1"
  echo "Line 2"
  echo "Line 3"
  echo "Line 4"
  echo "Line 5"
} > $DDIR/u1_doc.txt

{
  echo "Line 1"
  echo "Line 2"
  echo "Line 3"
  echo "Line 4"
  echo "Line 5"
} > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

> $DDIR/u1_doc.txt  # Delete all content

sleep 3

U1_LINES=$(wc -l < $DDIR/u1_doc.txt)
U2_LINES=$(wc -l < $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq 0 ] && [ "$U2_LINES" -eq 0 ]; then
    pass_test "Delete-all operation propagated correctly"
else
    fail_test "Delete-all failed (u1: $U1_LINES lines, u2: $U2_LINES lines)"
fi

# ============================================================================
# TEST 4: Non-Conflicting Edits
# ============================================================================
section "Test 4: Conflict Scenarios - Non-Conflicting Edits"
cleanup

printf "int x = 10;\nint y = 20;\nint z = 30;\n" > $DDIR/u1_doc.txt
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 modifies line 0
printf "int x = 50;\nint y = 20;\nint z = 30;\n" > $DDIR/u1_doc.txt
sleep 1

# u2 modifies line 1 (should have seen u1's x=50 by now)
printf "int x = 50;\nint y = 100;\nint z = 30;\n" > $DDIR/u2_doc.txt
sleep 2

L0_U1=$(sed -n '1p' $DDIR/u1_doc.txt)
L1_U1=$(sed -n '2p' $DDIR/u1_doc.txt)
L0_U2=$(sed -n '1p' $DDIR/u2_doc.txt)
L1_U2=$(sed -n '2p' $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$L0_U1" = "int x = 50;" ] && [ "$L1_U1" = "int y = 100;" ] && \
   [ "$L0_U2" = "int x = 50;" ] && [ "$L1_U2" = "int y = 100;" ]; then
    pass_test "Non-conflicting edits merged correctly"
else
    fail_test "Non-conflicting merge failed (u1: [$L0_U1,$L1_U1] u2: [$L0_U2,$L1_U2])"
fi

# ============================================================================
# TEST 5: Same line edited twice in a row (the later edit is the result)
# ============================================================================
section "Test 5: Conflict Scenarios - Sequential Same-Line Edits"
cleanup

printf "int x = 10;\nint y = 100;\nint z = 30;\n" > $DDIR/u1_doc.txt
printf "int x = 10;\nint y = 100;\nint z = 30;\n" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 modifies line 0 first
printf "int x = 75;\nint y = 100;\nint z = 30;\n" > $DDIR/u1_doc.txt
sleep 0.3

# u2 then edits the same line again, after seeing u1's edit
printf "int x = 99;\nint y = 100;\nint z = 30;\n" > $DDIR/u2_doc.txt
sleep 2

L0_U1=$(sed -n '1p' $DDIR/u1_doc.txt)
L0_U2=$(sed -n '1p' $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$L0_U1" = "int x = 99;" ] && [ "$L0_U2" = "int x = 99;" ]; then
    pass_test "Later edit of the same line is the result"
else
    fail_test "Sequential same-line edits failed (u1: $L0_U1, u2: $L0_U2)"
fi

# ============================================================================
# TEST 6: Structural Line Insert
# ============================================================================
section "Test 6: Conflict Scenarios - Line Insert"
cleanup

printf "int x = 1;\nint y = 2;\nint z = 3;\n" > $DDIR/u1_doc.txt
printf "int x = 1;\nint y = 2;\nint z = 3;\n" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 inserts a line in the middle
printf "int x = 1;\nint mid = 77;\nint y = 2;\nint z = 3;\n" > $DDIR/u1_doc.txt
sleep 2

U1_LINES=$(wc -l < $DDIR/u1_doc.txt)
U2_LINES=$(wc -l < $DDIR/u2_doc.txt)
L1_U2=$(sed -n '2p' $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq 4 ] && [ "$U2_LINES" -eq 4 ] && [ "$L1_U2" = "int mid = 77;" ]; then
    pass_test "Structural line insert propagated correctly"
else
    fail_test "Line insert failed (u1: $U1_LINES lines, u2: $U2_LINES lines)"
fi

# ============================================================================
# TEST 7: Structural Line Delete
# ============================================================================
section "Test 7: Conflict Scenarios - Line Delete"
cleanup

printf "int x = 11;\nint y = 22;\nint z = 33;\n" > $DDIR/u1_doc.txt
printf "int x = 11;\nint y = 22;\nint z = 33;\n" > $DDIR/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 deletes middle line
printf "int x = 11;\nint z = 33;\n" > $DDIR/u1_doc.txt
sleep 2

U1_LINES=$(wc -l < $DDIR/u1_doc.txt)
U2_LINES=$(wc -l < $DDIR/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq 2 ] && [ "$U2_LINES" -eq 2 ]; then
    pass_test "Structural line delete propagated correctly"
else
    fail_test "Line delete failed (u1: $U1_LINES lines, u2: $U2_LINES lines)"
fi

# ============================================================================
# TEST 8: LAN Mode (--host / --join over TCP)
# ============================================================================
section "Test 8: LAN Mode - Join, Sync & Access Code"
cleanup

LAN_PORT=${LAN_PORT:-9411}
LAN_ENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0"

printf 'alpha\nbeta\ngamma\n' > $DDIR/h1_doc.txt
rm -f $DDIR/j1_doc.txt $DDIR/j2_doc.txt

env $LAN_ENV SYNCTEXT_USER=h1 SYNCTEXT_PORT_BASE="$LAN_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --host "$DOC" secret > log_files/h1.log 2>&1 &
H1_PID=$!
sleep 1

env $LAN_ENV SYNCTEXT_USER=j1 SYNCTEXT_PORT_BASE="$LAN_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 secret > log_files/j1.log 2>&1 &
J1_PID=$!
env $LAN_ENV SYNCTEXT_USER=j2 SYNCTEXT_PORT_BASE="$LAN_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 secret > log_files/j2.log 2>&1 &
J2_PID=$!
sleep 1.5

# 8a: host snapshot reaches both joiners on connect
if diff -q $DDIR/h1_doc.txt $DDIR/j1_doc.txt >/dev/null 2>&1 && \
   diff -q $DDIR/h1_doc.txt $DDIR/j2_doc.txt >/dev/null 2>&1; then
    pass_test "LAN snapshot delivered to joiners on connect"
else
    fail_test "LAN snapshot not delivered to joiners"
fi

# 8b: host edit propagates to joiners
printf 'alpha\nbeta CHANGED\ngamma\ndelta\n' > $DDIR/h1_doc.txt
sleep 3
if diff -q $DDIR/h1_doc.txt $DDIR/j1_doc.txt >/dev/null 2>&1 && \
   diff -q $DDIR/h1_doc.txt $DDIR/j2_doc.txt >/dev/null 2>&1; then
    pass_test "LAN host edit propagated to joiners"
else
    fail_test "LAN host edit did not reach joiners"
fi

# 8c: joiner edit reaches the host and the other joiner (direct mesh links)
printf 'alpha\nbeta CHANGED\ngamma\ndelta\nepsilon\n' > $DDIR/j1_doc.txt
sleep 3
if diff -q $DDIR/j1_doc.txt $DDIR/h1_doc.txt >/dev/null 2>&1 && \
   diff -q $DDIR/j1_doc.txt $DDIR/j2_doc.txt >/dev/null 2>&1; then
    pass_test "LAN joiner edit reached host and peer joiner"
else
    fail_test "LAN joiner edit did not reach everyone (host/peer diverged)"
fi

# 8d: wrong access code is rejected
BAD_RC=0
env $LAN_ENV timeout 5 SYNCTEXT_USER=j3 SYNCTEXT_PORT_BASE="$LAN_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 wrongcode > log_files/j3.log 2>&1 || BAD_RC=$?
if [ "$BAD_RC" -ne 0 ]; then
    pass_test "LAN rejects joiner with wrong access code"
else
    fail_test "LAN accepted a joiner with wrong access code"
fi

kill $H1_PID $J1_PID $J2_PID 2>/dev/null || true
sleep 1

# ============================================================================
# TEST 9: LAN Mesh (direct links, losing any peer, no host to elect)
# ============================================================================
section "Test 9: LAN Mesh - Direct Links and Peer Loss"
cleanup

FO_PORT=$(( ${LAN_PORT:-9411} + 10 ))
FO_ENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=1"

printf 'alpha\nbeta\ngamma\n' > $DDIR/h1_doc.txt
rm -f $DDIR/j1_doc.txt $DDIR/j2_doc.txt

env $FO_ENV SYNCTEXT_USER=h1 SYNCTEXT_PORT_BASE="$FO_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --host "$DOC" secret > log_files/fo_h1.log 2> log_files/fo_h1.err &
FO_H1=$!
sleep 1
env $FO_ENV SYNCTEXT_USER=j1 SYNCTEXT_PORT_BASE="$FO_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 secret > log_files/fo_j1.log 2> log_files/fo_j1.err &
FO_J1=$!
sleep 0.8
env $FO_ENV SYNCTEXT_USER=j2 SYNCTEXT_PORT_BASE="$FO_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 secret > log_files/fo_j2.log 2> log_files/fo_j2.err &
FO_J2=$!
sleep 1.5

# 9a: everyone lists everyone, and the two joiners hold a direct link
ROSTER_J2=$(grep -a "Active users" log_files/fo_j2.log | tail -1 || true)
if echo "$ROSTER_J2" | grep -q "h1" && echo "$ROSTER_J2" | grep -q "j1" && echo "$ROSTER_J2" | grep -q "j2"; then
    pass_test "Every participant listed (h1, j1, j2)"
else
    fail_test "Participant list incomplete (saw: $ROSTER_J2)"
fi
if grep -q "linked with j2" log_files/fo_j1.err && grep -q "linked with j1" log_files/fo_j2.err; then
    pass_test "Joiners linked to each other directly (full mesh)"
else
    fail_test "Joiners have no direct link (j1: $(grep linked log_files/fo_j1.err | tr '\n' ' '))"
fi

# 9b: kill the participant that started the session; the others just drop it
kill -9 $FO_H1 2>/dev/null || true
wait $FO_H1 2>/dev/null || true
sleep 1

ROSTER_J1=$(grep -a "Active users" log_files/fo_j1.log | tail -1 || true)
ROSTER_J2=$(grep -a "Active users" log_files/fo_j2.log | tail -1 || true)
if ! echo "$ROSTER_J1$ROSTER_J2" | grep -q "h1" && echo "$ROSTER_J1" | grep -q "j2" && echo "$ROSTER_J2" | grep -q "j1"; then
    pass_test "Lost peer dropped; survivors still linked (no election)"
else
    fail_test "Peer loss not handled (j1: $ROSTER_J1 | j2: $ROSTER_J2)"
fi

# 9c: editing still syncs both ways, straight away
printf 'alpha\nbeta\ngamma\nfrom-j1\n' > $DDIR/j1_doc.txt
sleep 1
if diff -q $DDIR/j1_doc.txt $DDIR/j2_doc.txt >/dev/null 2>&1; then
    pass_test "Edit propagated after the first participant left"
else
    fail_test "Edit did not propagate after the first participant left"
fi

printf 'alpha\nbeta\ngamma\nfrom-j1\nfrom-j2\n' > $DDIR/j2_doc.txt
sleep 1
if diff -q $DDIR/j2_doc.txt $DDIR/j1_doc.txt >/dev/null 2>&1; then
    pass_test "Edit propagated in the other direction too"
else
    fail_test "Edit did not propagate in the other direction"
fi

# 9d: a newcomer can join through any participant, not only the one that hosted
J1_PORT=$(grep -o "linked with j1 (127.0.0.1:[0-9]*" log_files/fo_j2.err | head -1 | grep -o "[0-9]*$")
env $FO_ENV SYNCTEXT_USER=j3 SYNCTEXT_PORT_BASE="$J1_PORT" SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 secret > log_files/fo_j3.log 2> log_files/fo_j3.err &
FO_J3=$!
sleep 2
if [ -n "$J1_PORT" ] && diff -q $DDIR/j3_doc.txt $DDIR/j2_doc.txt >/dev/null 2>&1 && grep -q "linked with j2" log_files/fo_j3.err; then
    pass_test "Newcomer joined through a joiner and linked to everyone"
else
    fail_test "Joining through a joiner failed (port: $J1_PORT)"
fi

# 9e: down to the last participant: it carries on alone
kill -9 $FO_J1 $FO_J3 2>/dev/null || true
wait $FO_J1 $FO_J3 2>/dev/null || true
sleep 1
ROSTER_J2=$(grep -a "Active users" log_files/fo_j2.log | tail -1 || true)
if echo "$ROSTER_J2" | grep -q "j2" && ! echo "$ROSTER_J2" | grep -q "j1"; then
    pass_test "Last participant carries on alone"
else
    fail_test "Last participant did not notice it is alone (j2: $ROSTER_J2)"
fi

kill $FO_J2 2>/dev/null || true
sleep 1

# ============================================================================
# TEST 10: Built-in Terminal Editor (per-keystroke sync + peer carets)
# ============================================================================
section "Test 10: Built-in Terminal Editor"
cleanup

if ! command -v python3 >/dev/null 2>&1; then
    echo -e "${YELLOW}[SKIP]${NC} python3 not available; TUI test needs a pty harness"
else
    TUI_OUT=$(timeout 180 python3 tests/tui_pty_test.py 2>&1 || true)
    echo "$TUI_OUT" | grep -v "^RESULT " || true
    while IFS= read -r line; do
        case "$line" in
            "RESULT PASS "*) pass_test "${line#RESULT PASS }" ;;
            "RESULT FAIL "*) fail_test "${line#RESULT FAIL }" ;;
        esac
    done <<< "$TUI_OUT"
    if ! echo "$TUI_OUT" | grep -q "^RESULT "; then
        fail_test "TUI test produced no results"
    fi
fi

# ============================================================================
# TEST 11: Cross-directory startup sync
# ============================================================================
section "Test 11: Cross-Directory Startup Sync"
cleanup

XDIR="build/xdir_test"
XDOC="crossdir"
rm -rf "$XDIR"; mkdir -p "$XDIR"
REPO_ABS="$(pwd)"
mkdir -p "user_docs/$XDOC"
printf 'shared line\n' > "user_docs/$XDOC/x1_doc.txt"
rm -rf "$XDIR/user_docs/$XDOC"

XENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0"
env $XENV SYNCTEXT_USER=x1 "$REPO_ABS/build/editor" "$XDOC" > log_files/x1.log 2>&1 &
X1_PID=$!
sleep 1.5

printf 'shared line\nsecond line\n' > "user_docs/$XDOC/x1_doc.txt"
sleep 2

# started from a DIFFERENT working directory: it must still find the session's document
( cd "$XDIR" && env $XENV SYNCTEXT_USER=x2 "$REPO_ABS/build/editor" "$XDOC" > "$REPO_ABS/log_files/x2.log" 2>&1 ) &
X2_PID=$!
sleep 3

if diff -q "user_docs/$XDOC/x1_doc.txt" "$XDIR/user_docs/$XDOC/x2_doc.txt" >/dev/null 2>&1; then
    pass_test "Session started from another directory syncs on startup"
else
    fail_test "Cross-directory session did not sync (got: $(head -c 60 "$XDIR/user_docs/$XDOC/x2_doc.txt" 2>/dev/null))"
fi

printf 'shared line\nsecond line\nthird line\n' > "user_docs/$XDOC/x1_doc.txt"
sleep 2.5
if diff -q "user_docs/$XDOC/x1_doc.txt" "$XDIR/user_docs/$XDOC/x2_doc.txt" >/dev/null 2>&1; then
    pass_test "Later edits reach the cross-directory session"
else
    fail_test "Later edits did not reach the cross-directory session"
fi

kill $X1_PID $X2_PID 2>/dev/null || true
pkill -9 -f "$REPO_ABS/build/editor" 2>/dev/null || true
wait $X1_PID $X2_PID 2>/dev/null || true
rm -rf "$XDIR"
sleep 1

# ============================================================================
# TEST 12: Separate editor window
# ============================================================================
section "Test 12: Separate Editor Window"
cleanup

if ! command -v python3 >/dev/null 2>&1; then
    echo -e "${YELLOW}[SKIP]${NC} python3 not available; editor-window test needs a pty harness"
else
    EW_OUT=$(timeout 180 python3 tests/edit_window_test.py 2>&1 || true)
    echo "$EW_OUT" | grep -v "^RESULT " || true
    while IFS= read -r line; do
        case "$line" in
            "RESULT PASS "*) pass_test "${line#RESULT PASS }" ;;
            "RESULT FAIL "*) fail_test "${line#RESULT FAIL }" ;;
        esac
    done <<< "$EW_OUT"
    if ! echo "$EW_OUT" | grep -q "^RESULT "; then
        fail_test "Editor-window test produced no results"
    fi
fi

# ============================================================================
# TEST 13: Documents are separate by name
# ============================================================================
section "Test 13: Separate Documents"
cleanup
rm -rf user_docs/notes user_docs/plan

DENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_EDIT_WINDOW=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0"

# the same account works on two documents at once, and they stay apart
env $DENV SYNCTEXT_USER=a "$BIN" notes > log_files/d_na.log 2>&1 & D1=$!
env $DENV SYNCTEXT_USER=b "$BIN" notes > log_files/d_nb.log 2>&1 & D2=$!
env $DENV SYNCTEXT_USER=a "$BIN" plan  > log_files/d_ca.log 2>&1 & D3=$!
env $DENV SYNCTEXT_USER=b "$BIN" plan  > log_files/d_cb.log 2>&1 & D4=$!
sleep 2

LIVE=$(ps -o pid= -p $D1 $D2 $D3 $D4 2>/dev/null | wc -l)
if [ "$LIVE" -eq 4 ]; then
    pass_test "One account works on two documents at once"
else
    fail_test "Only $LIVE of 4 sessions stayed up"
fi

printf 'NOTES\nedit-in-notes\n' > user_docs/notes/a_doc.txt
sleep 3
if grep -q "edit-in-notes" user_docs/notes/b_doc.txt 2>/dev/null; then
    pass_test "Edits propagate within a document"
else
    fail_test "Edit did not reach the peer in the same document"
fi
if grep -q "edit-in-notes" user_docs/plan/a_doc.txt 2>/dev/null || \
   grep -q "edit-in-notes" user_docs/plan/b_doc.txt 2>/dev/null; then
    fail_test "Edit leaked into a different document"
else
    pass_test "Documents stay isolated from each other"
fi

kill $D1 $D2 $D3 $D4 2>/dev/null || true
sleep 1
kill -9 $D1 $D2 $D3 $D4 2>/dev/null || true

# unusable document names are refused rather than rewritten
BADOK=1
for bad in "." ".." "a/b" "a b"; do
    if env $DENV "$BIN" "$bad" >/dev/null 2>&1; then BADOK=0; fi
done
if env $DENV "$BIN" "$(printf 'x%.0s' $(seq 1 70))" >/dev/null 2>&1; then BADOK=0; fi
if [ "$BADOK" -eq 1 ]; then
    pass_test "Unsafe document names are rejected"
else
    fail_test "An unsafe document name was accepted"
fi

# a long name must not truncate the identity out of the queue name
cleanup
LONGID=$(printf 'd%.0s' $(seq 1 60))
rm -rf "user_docs/$LONGID"
env $DENV SYNCTEXT_USER=alice "$BIN" "$LONGID" > log_files/d_la.log 2>&1 & D6=$!
sleep 0.5
env $DENV SYNCTEXT_USER=bob   "$BIN" "$LONGID" > log_files/d_lb.log 2>&1 & D7=$!
sleep 2
QCOUNT=$(ls /dev/mqueue/ 2>/dev/null | grep -c synctext || true)
kill -9 $D6 $D7 2>/dev/null || true
rm -rf "user_docs/$LONGID"
if [ "$QCOUNT" -eq 2 ]; then
    pass_test "Long document name still gives each user its own queue"
else
    fail_test "Long document name collapsed users onto $QCOUNT queue(s)"
fi
rm -rf user_docs/notes user_docs/plan

# ============================================================================
# TEST 14: The access code selects the document
# ============================================================================
section "Test 14: Access Code Selects the Document"
cleanup
rm -rf user_docs/notes user_docs/plan

JENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_EDIT_WINDOW=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0"
JBASE=$(( ${LAN_PORT:-9411} + 390 ))

# one person shares two documents, each with its own code, on automatic ports
env $JENV SYNCTEXT_USER=user1 SYNCTEXT_PORT_BASE=$JBASE SYNCTEXT_PORT_SPAN=4 \
    "$BIN" --host notes codeN > log_files/j_h1.log 2>&1 & JH1=$!
sleep 1.5
env $JENV SYNCTEXT_USER=user1 SYNCTEXT_PORT_BASE=$JBASE SYNCTEXT_PORT_SPAN=4 \
    "$BIN" --host plan codeP > log_files/j_h2.log 2>&1 & JH2=$!
sleep 2

# a joiner presents only ip + code, and must land in the matching document
env $JENV SYNCTEXT_USER=user2 SYNCTEXT_PORT_BASE=$JBASE SYNCTEXT_PORT_SPAN=4 \
    "$BIN" --join 127.0.0.1 codeP > log_files/j_j1.log 2>&1 & JJ1=$!
sleep 4
if [ -f user_docs/plan/user2_doc.txt ] && [ ! -f user_docs/notes/user2_doc.txt ]; then
    pass_test "Access code picked the right document out of two"
else
    fail_test "Joiner landed in the wrong document"
fi

printf 'PLAN\nedit-in-plan\n' > user_docs/plan/user1_doc.txt
sleep 3
if grep -q "edit-in-plan" user_docs/plan/user2_doc.txt 2>/dev/null; then
    pass_test "Edits reach the joined document"
else
    fail_test "Edit did not reach the joiner"
fi
if grep -q "edit-in-plan" user_docs/notes/user1_doc.txt 2>/dev/null; then
    fail_test "Edit leaked into the other shared document"
else
    pass_test "The other shared document is untouched"
fi

# a code nobody is using is refused, not left hanging
NOCODE=$(env $JENV SYNCTEXT_PORT_BASE=$JBASE SYNCTEXT_PORT_SPAN=4 timeout 30 \
         "$BIN" --join 127.0.0.1 nosuchcode 2>&1 | tail -1)
if echo "$NOCODE" | grep -qi "no session\|access code"; then
    pass_test "An unknown access code is reported, not left hanging"
else
    fail_test "Unknown access code not reported (saw: $NOCODE)"
fi

kill $JH1 $JH2 $JJ1 2>/dev/null || true
sleep 1
kill -9 $JH1 $JH2 $JJ1 2>/dev/null || true
rm -rf user_docs/notes user_docs/plan

# ============================================================================
# TEST 15: Identities are assigned, never typed
# ============================================================================
section "Test 15: Automatic Identities"
cleanup
rm -rf user_docs/auto

AENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_EDIT_WINDOW=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0"

# the same account opens one document twice: the second is renamed automatically
env $AENV "$BIN" auto > log_files/a1.log 2>&1 & A1=$!
sleep 1.5
env $AENV "$BIN" auto > log_files/a2.log 2>&1 & A2=$!
sleep 2
NFILES=$(ls user_docs/auto/*_doc.txt 2>/dev/null | wc -l)
if [ "$NFILES" -eq 2 ]; then
    pass_test "Second session of one account gets its own identity"
else
    fail_test "Expected 2 participant files, found $NFILES"
fi

F1=$(ls user_docs/auto/*_doc.txt 2>/dev/null | head -1)
F2=$(ls user_docs/auto/*_doc.txt 2>/dev/null | tail -1)
printf 'one\ntwo\n' > "$F1"
sleep 3
if [ -n "$F2" ] && diff -q "$F1" "$F2" >/dev/null 2>&1; then
    pass_test "Two sessions with no arguments still sync"
else
    fail_test "Auto-named sessions did not sync"
fi
kill $A1 $A2 2>/dev/null || true
sleep 1
kill -9 $A1 $A2 2>/dev/null || true

# host and joiner on one machine share an account: the host must rename the joiner
cleanup
rm -rf user_docs/auto
ABASE=$(( ${LAN_PORT:-9411} + 420 ))
env $AENV SYNCTEXT_PORT_BASE=$ABASE SYNCTEXT_PORT_SPAN=1 "$BIN" --host auto pw > log_files/a_h.log 2>&1 & AH=$!
sleep 2
env $AENV SYNCTEXT_PORT_BASE=$ABASE SYNCTEXT_PORT_SPAN=1 "$BIN" --join 127.0.0.1 pw > log_files/a_j.log 2>&1 & AJ=$!
sleep 4
NF2=$(ls user_docs/auto/*_doc.txt 2>/dev/null | wc -l)
if [ "$NF2" -eq 2 ]; then
    pass_test "Host renames a joiner that arrives with the same identity"
else
    fail_test "Host and joiner shared one identity (files: $NF2)"
fi
HF=$(ls user_docs/auto/*_doc.txt 2>/dev/null | head -1)
printf 'shared\nfrom-one-side\n' > "$HF"
sleep 3
BOTH=$(grep -l "from-one-side" user_docs/auto/*_doc.txt 2>/dev/null | wc -l)
if [ "$BOTH" -eq 2 ]; then
    pass_test "Distinct identities let both sides see each other's edits"
else
    fail_test "Edit reached $BOTH of 2 participants"
fi
kill $AH $AJ 2>/dev/null || true
sleep 1
kill -9 $AH $AJ 2>/dev/null || true
rm -rf user_docs/auto

# ============================================================================
# TEST 16: Concurrent edits through files (the CRDT's main job)
# ============================================================================
section "Test 16: Concurrent Edits (files, three users at once)"
cleanup

CENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=1"
printf 'int x = 10;\nint y = 20;\nint z = 30;\n' > $DDIR/u1_doc.txt
for U in u1 u2 u3; do
    env $CENV SYNCTEXT_USER=$U "$BIN" "$DOC" > log_files/cc_$U.log 2>&1 &
    eval "C_$U=\$!"
    sleep 0.5
done
sleep 1.5

# all three save at the same instant, two of them on the same line
printf 'int x = 10; // u1\nint y = 20;\nint z = 30;\n' > $DDIR/u1_doc.txt & W1=$!
printf '// u2\nint x = 10;\nint y = 20;\nint z = 30;\n' > $DDIR/u2_doc.txt & W2=$!
printf 'int x = 99;\nint y = 20;\nint z = 30;\n' > $DDIR/u3_doc.txt & W3=$!
wait $W1 $W2 $W3     # only the writers; the editors run on in the background
sleep 4

EXPECT_CC=$'// u2\nint x = 99; // u1\nint y = 20;\nint z = 30;'
if diff -q $DDIR/u1_doc.txt $DDIR/u2_doc.txt >/dev/null && diff -q $DDIR/u1_doc.txt $DDIR/u3_doc.txt >/dev/null; then
    pass_test "Three simultaneous saves converge to one document"
else
    fail_test "Simultaneous saves diverged"
fi
if [ "$(cat $DDIR/u1_doc.txt)" = "$EXPECT_CC" ]; then
    pass_test "Every concurrent edit is kept, including two on the same line"
else
    fail_test "Concurrent edits lost (got: $(tr '\n' '|' < $DDIR/u1_doc.txt))"
fi
kill $C_u1 $C_u2 $C_u3 2>/dev/null || true
sleep 1

# ============================================================================
# TEST 17: Lost messages are repaired (anti-entropy)
# ============================================================================
section "Test 17: Lost Messages Are Repaired"
cleanup

printf 'start\n' > $DDIR/u1_doc.txt
# u1 gives up on a full queue immediately, so messages to a stalled peer are lost
env $CENV SYNCTEXT_DEBUG_LEVEL=3 SYNCTEXT_SEND_RETRY_COUNT=0 SYNCTEXT_USER=u1 "$BIN" "$DOC" > log_files/ae_u1.log 2>&1 &
AE1=$!
sleep 0.5
env $CENV SYNCTEXT_USER=u2 "$BIN" "$DOC" > log_files/ae_u2.log 2>&1 &
AE2=$!
sleep 1.5

kill -STOP $AE2                 # u2 stops reading its queue (10 messages deep)
for i in $(seq 1 25); do
    printf 'start\n' > $DDIR/tmp_ae.txt
    for k in $(seq 1 $i); do echo "line $k" >> $DDIR/tmp_ae.txt; done
    cp $DDIR/tmp_ae.txt $DDIR/u1_doc.txt
    sleep 0.15
done
rm -f $DDIR/tmp_ae.txt
sleep 0.5
kill -CONT $AE2
sleep 6

if grep -aq "mq_send failed after retries" log_files/ae_u1.log; then
    pass_test "Messages to the stalled peer were really dropped"
else
    fail_test "Expected dropped messages while the peer was stopped"
fi
if diff -q $DDIR/u1_doc.txt $DDIR/u2_doc.txt >/dev/null 2>&1 && grep -q "^line 25$" $DDIR/u2_doc.txt; then
    pass_test "The stalled peer caught up on every edit it missed"
else
    fail_test "Lost edits were not repaired (u2 has $(wc -l < $DDIR/u2_doc.txt) lines)"
fi
kill $AE1 $AE2 2>/dev/null || true
sleep 1

# ============================================================================
# TEST 18: Concurrent edits over LAN, and edits made while a peer is going down
# ============================================================================
section "Test 18: LAN Concurrent Edits and Peer Loss"
cleanup

CC_PORT=${CC_PORT:-9431}
LENV="SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_TUI=0 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_PORT_BASE=$CC_PORT SYNCTEXT_PORT_SPAN=1"
printf 'red\ngreen\nblue\n' > $DDIR/h1_doc.txt
rm -f $DDIR/j1_doc.txt $DDIR/j2_doc.txt
env $LENV SYNCTEXT_USER=h1 "$BIN" --host "$DOC" cccode > log_files/lc_h1.log 2>&1 & LH=$!
sleep 1
env $LENV SYNCTEXT_USER=j1 "$BIN" --join 127.0.0.1 cccode > log_files/lc_j1.log 2>&1 & LJ1=$!
sleep 0.5
env $LENV SYNCTEXT_USER=j2 "$BIN" --join 127.0.0.1 cccode > log_files/lc_j2.log 2>&1 & LJ2=$!
sleep 2

printf 'red!\ngreen\nblue\n' > $DDIR/h1_doc.txt & W1=$!
printf 'red?\ngreen\nblue\n' > $DDIR/j1_doc.txt & W2=$!
printf 'red\ngreen\nblue\nwhite\n' > $DDIR/j2_doc.txt & W3=$!
wait $W1 $W2 $W3
sleep 4
if diff -q $DDIR/h1_doc.txt $DDIR/j1_doc.txt >/dev/null && diff -q $DDIR/h1_doc.txt $DDIR/j2_doc.txt >/dev/null && \
   grep -q '^red[!?][!?]$' $DDIR/h1_doc.txt && grep -q '^white$' $DDIR/h1_doc.txt; then
    pass_test "Simultaneous LAN edits (two on one line) converge with all edits kept"
else
    fail_test "LAN concurrent edits diverged or lost text (h1: $(tr '\n' '|' < $DDIR/h1_doc.txt) j1: $(tr '\n' '|' < $DDIR/j1_doc.txt))"
fi

kill -9 $LH 2>/dev/null || true
# j2 saves straight away, while the peer it joined through is going down
printf '%s\nduring-failover\n' "$(cat $DDIR/j2_doc.txt)" > $DDIR/j2_doc.txt
sleep 2   # no election to wait for
printf 'top\n%s\n' "$(cat $DDIR/j1_doc.txt)" > $DDIR/j1_doc.txt
sleep 4
if diff -q $DDIR/j1_doc.txt $DDIR/j2_doc.txt >/dev/null && grep -q '^during-failover$' $DDIR/j1_doc.txt && \
   grep -q '^top$' $DDIR/j2_doc.txt; then
    pass_test "An edit made as a peer went down still reaches the survivors"
else
    fail_test "Failover lost an edit (j1: $(tr '\n' '|' < $DDIR/j1_doc.txt) j2: $(tr '\n' '|' < $DDIR/j2_doc.txt))"
fi
kill $LJ1 $LJ2 2>/dev/null || true
sleep 1

# ============================================================================
# TEST 19: Concurrent typing in the built-in editor
# ============================================================================
section "Test 19: Concurrent Typing in the Built-in Editor"
cleanup
rm -rf user_docs/cc_ends user_docs/cc_spot user_docs/cc_three user_docs/cc_caret

if ! command -v python3 >/dev/null 2>&1; then
    echo -e "${YELLOW}[SKIP]${NC} python3 not available; needs a pty harness"
else
    CC_OUT=$(timeout 240 python3 tests/concurrency_test.py 2>&1 || true)
    while IFS= read -r line; do
        case "$line" in
            "RESULT PASS "*) pass_test "${line#RESULT PASS }" ;;
            "RESULT FAIL "*) fail_test "${line#RESULT FAIL }" ;;
        esac
    done <<< "$CC_OUT"
    if ! echo "$CC_OUT" | grep -q "^RESULT "; then
        fail_test "Concurrency test produced no results"
    fi
fi
rm -rf user_docs/cc_ends user_docs/cc_spot user_docs/cc_three user_docs/cc_caret

# ============================================================================
# SUMMARY
# ============================================================================
cleanup

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║                    TEST SUMMARY                            ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

TOTAL_TESTS=$((PASS_TOTAL + FAIL_TOTAL))

if [ $FAIL_TOTAL -eq 0 ]; then
    echo -e "${GREEN}✅ ALL TESTS PASSED${NC}"
else
    echo -e "${RED}❌ SOME TESTS FAILED${NC}"
fi

echo ""
echo -e "Total Tests: $TOTAL_TESTS"
echo -e "${GREEN}Passed: $PASS_TOTAL${NC}"
echo -e "${RED}Failed: $FAIL_TOTAL${NC}"
echo ""

if [ $FAIL_TOTAL -gt 0 ]; then
    echo "Check log files in log_files/ for details"
    exit 1
fi

exit 0
