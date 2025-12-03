#!/bin/bash
# Test: Insert 50 lines to stress-test the queue flow control

pkill -9 -f "./build/editor" 2>/dev/null
rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry user_docs/u1_doc.txt user_docs/u2_doc.txt log_files/u1.log log_files/u2.log 2>/dev/null
sleep 0.5

# Create initial files
echo "Line 0" > user_docs/u1_doc.txt
echo "Line 0" > user_docs/u2_doc.txt

# Start editors WITHOUT debug logging (to not slow things down)
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 ./build/editor u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 ./build/editor u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

# Write 50 lines
{
  echo "Line 0"
  for i in $(seq 1 50); do
    echo "Line $i"
  done
} > user_docs/u1_doc.txt

sleep 3

# Kill editors
kill $U1_PID $U2_PID 2>/dev/null
sleep 1

echo "=== Line count ==="
U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)
echo "u1: $U1_LINES lines"
echo "u2: $U2_LINES lines"

if [ "$U1_LINES" -eq "$U2_LINES" ]; then
    echo "[PASS] Line counts match"
    
    # Verify all lines are present
    MISSING=0
    for i in $(seq 0 50); do
        if ! grep -q "^Line $i$" user_docs/u2_doc.txt; then
            echo "[FAIL] Line $i is missing"
            MISSING=$((MISSING + 1))
        fi
    done
    
    if [ $MISSING -eq 0 ]; then
        echo "[PASS] All 51 lines present in u2"
    else
        echo "[FAIL] $MISSING lines are missing"
    fi
else
    echo "[FAIL] Line counts differ!"
    echo ""
    echo "=== Missing lines in u2 ==="
    for i in $(seq 0 50); do
        if ! grep -q "^Line $i$" user_docs/u2_doc.txt; then
            echo "Line $i"
        fi
    done
fi
