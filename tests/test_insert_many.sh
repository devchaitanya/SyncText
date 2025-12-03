#!/usr/bin/env bash
# Test insert many lines

set -e
BIN="./build/editor"

cleanup() {
    pkill -9 -f "$BIN" 2>/dev/null || true
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
}

cleanup
sleep 1

echo "[test] Creating initial file with 1 line..."
cat > user_docs/u1_doc.txt <<EOF
initial line
EOF
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting u1 editor..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=3 "$BIN" u1 > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 1

echo "[test] Starting u2 editor..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=3 "$BIN" u2 > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 2

echo "[test] Initial state:"
echo "user_docs/u1_doc.txt: $(wc -l < user_docs/u1_doc.txt) lines"
echo "user_docs/u2_doc.txt: $(wc -l < user_docs/u2_doc.txt) lines"

echo ""
echo "[test] Inserting 20 lines into user_docs/u1_doc.txt..."
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
sleep 3

echo ""
echo "[test] Final state:"
u1_lines=$(wc -l < user_docs/u1_doc.txt)
u2_lines=$(wc -l < user_docs/u2_doc.txt)
echo "user_docs/u1_doc.txt: $u1_lines lines"
echo "user_docs/u2_doc.txt: $u2_lines lines"

echo ""
if [ "$u2_lines" -eq 21 ]; then
    echo "[PASS] u2 correctly has 21 lines"
else
    echo "[FAIL] u2 has $u2_lines lines (should be 21)"
    echo ""
    echo "user_docs/u2_doc.txt content:"
    cat user_docs/u2_doc.txt
fi

cleanup
