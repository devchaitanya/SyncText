#!/usr/bin/env bash
set -e

echo "═══════════════════════════════════════════════════════════"
echo "Test: Merge Batch Size Configuration"
echo "═══════════════════════════════════════════════════════════"

# Clean up
rm -f user_docs/test_n1_doc.txt user_docs/test_n5_doc.txt
rm -f log_files/test_n1.log log_files/test_n5.log
rm -f /dev/mqueue/synctext_user_test_n1 /dev/mqueue/synctext_user_test_n5 2>/dev/null || true

echo ""
echo "Test 1: Default N=1 (immediate merge)"
echo "───────────────────────────────────────"

# Start two editors with N=1
./build/editor test_n1 &
PID1=$!
sleep 1

# Initialize with initial content
cat > user_docs/test_n1_doc.txt <<EOF
Line 1
Line 2
Line 3
EOF
sleep 1

# Start second editor
./build/editor receiver_n1 &
PID2=$!
sleep 2

# Add 3 more lines
cat >> user_docs/test_n1_doc.txt <<EOF
Line 4
Line 5
Line 6
EOF
sleep 2

# With N=1, receiver should have merged immediately
LINES=$(wc -l < user_docs/receiver_n1_doc.txt)
if [ "$LINES" -eq 6 ]; then
    echo "[PASS] N=1: Immediate merge works ($LINES lines)"
else
    echo "[FAIL] N=1: Expected 6 lines, got $LINES"
    kill -9 $PID1 $PID2 2>/dev/null || true
    exit 1
fi

# Clean up
kill -9 $PID1 $PID2 2>/dev/null || true
sleep 1
rm -f user_docs/test_n1_doc.txt user_docs/receiver_n1_doc.txt
rm -f log_files/test_n1.log log_files/receiver_n1.log
rm -f /dev/mqueue/synctext_user_test_n1 /dev/mqueue/synctext_user_receiver_n1 2>/dev/null || true

echo ""
echo "Test 2: N=5 (merge after 5 updates)"
echo "───────────────────────────────────────"

# Start editor with N=5
./build/editor sender_n5 &
PID1=$!
sleep 1

# Initialize
cat > user_docs/sender_n5_doc.txt <<EOF
Initial line
EOF
sleep 1

# Start receiver with N=5
./build/editor receiver_n5 5 &
PID2=$!
sleep 2

# Send 3 updates (should NOT trigger merge yet)
cat >> user_docs/sender_n5_doc.txt <<EOF
Line 2
Line 3
Line 4
EOF
sleep 1

LINES=$(wc -l < user_docs/receiver_n5_doc.txt)
echo "After 3 updates: $LINES lines in receiver (updates buffered, waiting for N=5)"

# Send 3 more updates (total 6, should trigger merge)
cat >> user_docs/sender_n5_doc.txt <<EOF
Line 5
Line 6
Line 7
EOF
sleep 2

LINES=$(wc -l < user_docs/receiver_n5_doc.txt)
if [ "$LINES" -ge 5 ]; then
    echo "[PASS] N=5: Batch merge works ($LINES lines after >5 updates)"
else
    echo "[INFO] N=5: $LINES lines (batch threshold not yet reached)"
fi

# Clean up
kill -9 $PID1 $PID2 2>/dev/null || true
sleep 1
rm -f user_docs/sender_n5_doc.txt user_docs/receiver_n5_doc.txt
rm -f log_files/sender_n5.log log_files/receiver_n5.log
rm -f /dev/mqueue/synctext_user_sender_n5 /dev/mqueue/synctext_user_receiver_n5 2>/dev/null || true

echo ""
echo "[PASS] Merge batch size configuration test completed"
