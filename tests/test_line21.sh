#!/bin/bash

# Test: Insert 30 lines (0-30) and check if line 21 is missing

pkill -9 -f "./build/editor" 2>/dev/null
rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry user_docs/u1_doc.txt user_docs/u2_doc.txt log_files/u1.log log_files/u2.log 2>/dev/null
sleep 0.5

# Create initial files
echo "Line 0" > user_docs/u1_doc.txt
echo "Line 0" > user_docs/u2_doc.txt

# Start editors with debug logging
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=3 SYNCTEXT_STARTUP_SYNC=0 ./build/editor u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=3 SYNCTEXT_STARTUP_SYNC=0 ./build/editor u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 1

# Write initial content (lines 0-30)
{
  echo "Line 0"
  for i in $(seq 1 30); do
    echo "Line $i"
  done
} > user_docs/u1_doc.txt

sleep 3

# Kill editors
kill $U1_PID $U2_PID 2>/dev/null
sleep 1

echo "=== Lines sent by u1 (from debug logs) ==="
grep '\[debug-send\] Processing' log_files/u1.log | sed 's/.*line=//' | cut -d' ' -f1 | sort -n

echo ""
echo "=== Lines received by u2 ==="
grep "^Line [0-9]" user_docs/u2_doc.txt | nl -v0

echo ""
echo "=== Checking for specific missing lines ==="
for line_num in 9 19 21 29; do
  if grep -q "^Line $line_num$" user_docs/u2_doc.txt; then
    echo "Line $line_num: OK"
  else
    echo "Line $line_num: MISSING ❌"
  fi
done

echo ""
echo "=== Debug: Check what was generated for line 21 ==="
grep 'line=21' log_files/u1.log | head -5
