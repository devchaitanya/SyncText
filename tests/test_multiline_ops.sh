#!/usr/bin/env bash
# Test multi-line inserts and deletes

set -e
BIN="./build/editor"

cleanup_ipc() {
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
}

cleanup_ipc
pkill -9 -f "$BIN" 2>/dev/null || true
sleep 1

echo "[test] Test 1: Delete all lines in u1, check u2"
cat > user_docs/u1_doc.txt <<EOF
Line 1
Line 2
Line 3
Line 4
Line 5
EOF
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting editors..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u1_doc.txt > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 0.5
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u2_doc.txt > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 1.5

echo "[test] u1 deletes all lines..."
> user_docs/u1_doc.txt
sleep 2

echo "[test] Checking u2..."
u2_lines=$(wc -l < user_docs/u2_doc.txt)
echo "u2 has $u2_lines lines"
cat user_docs/u2_doc.txt

if [ "$u2_lines" -eq 0 ]; then
    echo "[PASS] Test 1: u2 correctly shows 0 lines"
else
    echo "[FAIL] Test 1: u2 still has $u2_lines lines (should be 0)"
fi

kill $u1_pid $u2_pid 2>/dev/null || true
sleep 1

echo ""
echo "[test] Test 2: Insert many lines in u1, check u2"
cleanup_ipc

cat > user_docs/u1_doc.txt <<EOF
initial line
EOF
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting editors..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u1_doc.txt > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 0.5
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u2_doc.txt > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 1.5

echo "[test] u1 inserts 20 new lines..."
cat > user_docs/u1_doc.txt <<EOF
initial line
Line 1
Line 2
Line 3
Line 4
Line 5
Line 6
Line 7
Line 8
Line 9
Line 10
Line 11
Line 12
Line 13
Line 14
Line 15
Line 16
Line 17
Line 18
Line 19
Line 20
EOF
sleep 2

echo "[test] Checking u2..."
u2_lines=$(wc -l < user_docs/u2_doc.txt)
echo "u2 has $u2_lines lines (should be 21)"
echo "--- user_docs/u2_doc.txt content ---"
cat user_docs/u2_doc.txt
echo "--- end ---"

if [ "$u2_lines" -eq 21 ]; then
    echo "[PASS] Test 2: u2 correctly has 21 lines"
else
    echo "[FAIL] Test 2: u2 has $u2_lines lines (should be 21)"
fi

kill $u1_pid $u2_pid 2>/dev/null || true
sleep 1

echo ""
echo "[test] Test 3: u1 deletes all, u2 inserts all (race condition)"
cleanup_ipc

cat > user_docs/u1_doc.txt <<EOF
A
B
C
D
E
EOF
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting editors..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u1_doc.txt > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 0.5
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=0 "$BIN" user_docs/u2_doc.txt > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 1.5

echo "[test] u1 deletes all, u2 adds many lines (simultaneously)..."
> user_docs/u1_doc.txt &
cat > user_docs/u2_doc.txt <<EOF
A
B
C
D
E
New 1
New 2
New 3
New 4
New 5
New 6
New 7
New 8
New 9
New 10
EOF
wait
sleep 2

echo "[test] Checking convergence..."
u1_lines=$(wc -l < user_docs/u1_doc.txt)
u2_lines=$(wc -l < user_docs/u2_doc.txt)
echo "u1 has $u1_lines lines"
echo "u2 has $u2_lines lines"

echo "--- user_docs/u1_doc.txt ---"
cat user_docs/u1_doc.txt
echo "--- user_docs/u2_doc.txt ---"
cat user_docs/u2_doc.txt

if [ "$u1_lines" -eq "$u2_lines" ]; then
    echo "[PASS] Test 3: Line counts match"
else
    echo "[FAIL] Test 3: Line counts differ ($u1_lines vs $u2_lines)"
fi

kill $u1_pid $u2_pid 2>/dev/null || true
cleanup_ipc

echo ""
echo "=== Summary ==="
