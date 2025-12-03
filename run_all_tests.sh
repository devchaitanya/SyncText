#!/bin/bash
# SyncText - Comprehensive Test Suite
# Runs all major tests in sequence and reports results

set -e

BIN="./build/editor"
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
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
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

echo "int x = 10;" > user_docs/u1_doc.txt
echo "int x = 10;" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

cat > user_docs/u1_doc.txt << 'EOF'
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

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq "$U2_LINES" ] && [ "$U1_LINES" -eq 13 ]; then
    U1_LINE9=$(sed -n '9p' user_docs/u1_doc.txt)
    U2_LINE9=$(sed -n '9p' user_docs/u2_doc.txt)
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

echo "Line 0" > user_docs/u1_doc.txt
echo "Line 0" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

{
  echo "Line 0"
  for i in $(seq 1 50); do
    echo "Line $i"
  done
} > user_docs/u1_doc.txt

sleep 3

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq "$U2_LINES" ] && [ "$U1_LINES" -eq 51 ]; then
    # Check for specific lines that previously failed
    MISSING=0
    for line_num in 11 23; do
        if ! grep -q "^Line $line_num$" user_docs/u2_doc.txt; then
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
} > user_docs/u1_doc.txt

{
  echo "Line 1"
  echo "Line 2"
  echo "Line 3"
  echo "Line 4"
  echo "Line 5"
} > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

> user_docs/u1_doc.txt  # Delete all content

sleep 3

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)

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

printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u1_doc.txt
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u1 > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u2 > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 modifies line 0
printf "int x = 50;\nint y = 20;\nint z = 30;\n" > user_docs/u1_doc.txt
sleep 1

# u2 modifies line 1 (should have seen u1's x=50 by now)
printf "int x = 50;\nint y = 100;\nint z = 30;\n" > user_docs/u2_doc.txt
sleep 2

L0_U1=$(sed -n '1p' user_docs/u1_doc.txt)
L1_U1=$(sed -n '2p' user_docs/u1_doc.txt)
L0_U2=$(sed -n '1p' user_docs/u2_doc.txt)
L1_U2=$(sed -n '2p' user_docs/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$L0_U1" = "int x = 50;" ] && [ "$L1_U1" = "int y = 100;" ] && \
   [ "$L0_U2" = "int x = 50;" ] && [ "$L1_U2" = "int y = 100;" ]; then
    pass_test "Non-conflicting edits merged correctly"
else
    fail_test "Non-conflicting merge failed (u1: [$L0_U1,$L1_U1] u2: [$L0_U2,$L1_U2])"
fi

# ============================================================================
# TEST 5: Conflicting Edits (LWW)
# ============================================================================
section "Test 5: Conflict Scenarios - LWW Winner"
cleanup

printf "int x = 10;\nint y = 100;\nint z = 30;\n" > user_docs/u1_doc.txt
printf "int x = 10;\nint y = 100;\nint z = 30;\n" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u1 > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u2 > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 modifies line 0 first
printf "int x = 75;\nint y = 100;\nint z = 30;\n" > user_docs/u1_doc.txt
sleep 0.3

# u2 modifies same line 0 (should win via LWW)
printf "int x = 99;\nint y = 100;\nint z = 30;\n" > user_docs/u2_doc.txt
sleep 2

L0_U1=$(sed -n '1p' user_docs/u1_doc.txt)
L0_U2=$(sed -n '1p' user_docs/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$L0_U1" = "int x = 99;" ] && [ "$L0_U2" = "int x = 99;" ]; then
    pass_test "LWW conflict resolution works correctly"
else
    fail_test "LWW conflict resolution failed (u1: $L0_U1, u2: $L0_U2)"
fi

# ============================================================================
# TEST 6: Structural Line Insert
# ============================================================================
section "Test 6: Conflict Scenarios - Line Insert"
cleanup

printf "int x = 1;\nint y = 2;\nint z = 3;\n" > user_docs/u1_doc.txt
printf "int x = 1;\nint y = 2;\nint z = 3;\n" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u1 > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u2 > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 inserts a line in the middle
printf "int x = 1;\nint mid = 77;\nint y = 2;\nint z = 3;\n" > user_docs/u1_doc.txt
sleep 2

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)
L1_U2=$(sed -n '2p' user_docs/u2_doc.txt)

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

printf "int x = 11;\nint y = 22;\nint z = 33;\n" > user_docs/u1_doc.txt
printf "int x = 11;\nint y = 22;\nint z = 33;\n" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u1 > log_files/conflict_u1.log 2>&1 &
U1_PID=$!
sleep 0.4

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=1 SYNCTEXT_MERGE_WRITE_GRACE_MS=300 SYNCTEXT_STARTUP_SYNC=1 "$BIN" u2 > log_files/conflict_u2.log 2>&1 &
U2_PID=$!
sleep 1

# u1 deletes middle line
printf "int x = 11;\nint z = 33;\n" > user_docs/u1_doc.txt
sleep 2

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)

kill $U1_PID $U2_PID 2>/dev/null || true
sleep 0.5

if [ "$U1_LINES" -eq 2 ] && [ "$U2_LINES" -eq 2 ]; then
    pass_test "Structural line delete propagated correctly"
else
    fail_test "Line delete failed (u1: $U1_LINES lines, u2: $U2_LINES lines)"
fi

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
