#!/usr/bin/env bash
# Test delete all lines

set -e
BIN="./build/editor"

cleanup() {
    pkill -9 -f "$BIN" 2>/dev/null || true
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
}

cleanup
sleep 1

echo "[test] Creating initial files with 5 lines..."
cat > user_docs/u1_doc.txt <<EOF
Line 1
Line 2
Line 3
Line 4
Line 5
EOF
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting u1 editor..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=2 "$BIN" u1 > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 1

echo "[test] Starting u2 editor..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=2 "$BIN" u2 > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 2

echo "[test] Initial state:"
echo "user_docs/u1_doc.txt: $(wc -l < user_docs/u1_doc.txt) lines"
echo "user_docs/u2_doc.txt: $(wc -l < user_docs/u2_doc.txt) lines"

echo ""
echo "[test] Deleting ALL lines from user_docs/u1_doc.txt..."
> user_docs/u1_doc.txt
sleep 3

echo ""
echo "[test] Final state:"
u1_lines=$(wc -l < user_docs/u1_doc.txt)
u2_lines=$(wc -l < user_docs/u2_doc.txt)
echo "user_docs/u1_doc.txt: $u1_lines lines"
echo "user_docs/u2_doc.txt: $u2_lines lines"

echo ""
echo "--- user_docs/u1_doc.txt content ---"
cat user_docs/u1_doc.txt || echo "(empty)"
echo "--- user_docs/u2_doc.txt content ---"
cat user_docs/u2_doc.txt || echo "(empty)"

echo ""
if [ "$u2_lines" -eq 0 ]; then
    echo "[PASS] u2 correctly has 0 lines"
else
    echo "[FAIL] u2 has $u2_lines lines (should be 0)"
    echo ""
    echo "Checking log_files/u1.log for sent messages:"
    grep -i "sent\|broadcast\|type=" log_files/u1.log | tail -20
    echo ""
    echo "Checking log_files/u2.log for received messages:"
    grep -i "received\|merge\|type=" log_files/u2.log | tail -20
fi

cleanup
