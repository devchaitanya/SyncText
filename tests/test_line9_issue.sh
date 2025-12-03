#!/usr/bin/env bash
# Debug why Line 9 is missing

set -e
BIN="./build/editor"

cleanup() {
    pkill -9 -f "$BIN" 2>/dev/null || true
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
}

cleanup
sleep 1

echo "[test] Creating file with 1 line..."
echo "Line 0" > user_docs/u1_doc.txt
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

echo "[test] Starting editors..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=3 "$BIN" u1 > log_files/u1.log 2>&1 &
u1_pid=$!
sleep 1
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=3 "$BIN" u2 > log_files/u2.log 2>&1 &
u2_pid=$!
sleep 2

echo "[test] Adding 10 lines..."
cat > user_docs/u1_doc.txt <<'EOF'
Line 0
Line 1
Line 2
Line 3
Line 4
Line 5
Line 6
Line 7
Line 8
Line 9
EOF

sleep 3

echo ""
echo "=== Checking what u1 sent ==="
grep "sent msg" log_files/u1.log | grep "type=3" | grep -oE "line=[0-9]+" | sort -t= -k2 -n | uniq

echo ""
echo "=== Checking what u2 received ==="
grep "received msg" log_files/u2.log | grep "type=3" | grep -oE "line=[0-9]+" | sort -t= -k2 -n | uniq

echo ""
echo "=== Final state ==="
echo "u1: $(wc -l < user_docs/u1_doc.txt) lines"
echo "u2: $(wc -l < user_docs/u2_doc.txt) lines"

echo ""
echo "user_docs/u2_doc.txt:"
cat user_docs/u2_doc.txt

cleanup
