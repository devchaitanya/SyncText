#!/bin/bash
# Debug Scenario 1 with detailed logging

BIN="./build/editor"

# Clean state
pkill -9 -f "$BIN" 2>/dev/null
rm -f user_docs/u1_doc.txt user_docs/u2_doc.txt *.log /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null
sleep 1

# Prepare baseline
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u1_doc.txt
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u2_doc.txt

echo "[test] Starting u1..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 \
SYNCTEXT_MERGE_WRITE_GRACE_MS=200 SYNCTEXT_STARTUP_SYNC=0 \
  "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.5

echo "[test] Starting u2..."
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 \
SYNCTEXT_MERGE_WRITE_GRACE_MS=200 SYNCTEXT_STARTUP_SYNC=0 \
  "$BIN" u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

echo "[test] u1 changes x: 10 -> 50"
sed -i '1s@.*@int x = 50;@' user_docs/u1_doc.txt
sleep 0.5

echo "[test] u2 changes y: 20 -> 100"
sed -i '2s@.*@int y = 100;@' user_docs/u2_doc.txt
sleep 3

echo ""
echo "=== Final state ==="
echo "user_docs/u1_doc.txt:"
cat user_docs/u1_doc.txt
echo ""
echo "user_docs/u2_doc.txt:"
cat user_docs/u2_doc.txt

echo ""
echo "=== Did u1 receive u2's update? ==="
if grep -q "int y = 100;" user_docs/u1_doc.txt; then
    echo "YES - u1 has y=100"
else
    echo "NO - u1 still has y=20"
    echo ""
    echo "=== u1 log (checking for u2's message) ==="
    grep -E "received msg from user=u2|Line 1:" log_files/u1.log | tail -10
fi

echo ""
echo "=== Did u2 receive u1's update? ==="
if grep -q "int x = 50;" user_docs/u2_doc.txt; then
    echo "YES - u2 has x=50"
else
    echo "NO - u2 still has x=10"
fi

kill $U1_PID $U2_PID 2>/dev/null
