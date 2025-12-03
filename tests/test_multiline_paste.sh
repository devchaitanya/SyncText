#!/bin/bash
# Test multi-line paste issue

BIN="./build/editor"
FILE_U1="user_docs/u1_doc.txt"
FILE_U2="user_docs/u2_doc.txt"

# Start with empty files
echo "int x = 10;" > "$FILE_U1"
echo "int x = 10;" > "$FILE_U2"

# Start editors with debug logging
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=2 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=2 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
U2_PID=$!
sleep 0.5

echo "Started u1 (pid $U1_PID) and u2 (pid $U2_PID)"

# Wait for startup
sleep 1

# Paste 13 lines into u1's file
cat > "$FILE_U1" << 'EOF'
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

echo "Pasted 13 lines into $FILE_U1"

# Wait for propagation
sleep 3

echo ""
echo "=== user_docs/u1_doc.txt ==="
cat -n "$FILE_U1"

echo ""
echo "=== user_docs/u2_doc.txt ==="
cat -n "$FILE_U2"

echo ""
echo "=== Line count comparison ==="
U1_LINES=$(wc -l < "$FILE_U1")
U2_LINES=$(wc -l < "$FILE_U2")
echo "u1: $U1_LINES lines"
echo "u2: $U2_LINES lines"

if [ "$U1_LINES" -eq "$U2_LINES" ]; then
    echo "[PASS] Line counts match"
else
    echo "[FAIL] Line counts differ!"
fi

# Show debug info about line 9
echo ""
echo "=== Line 9 comparison ==="
echo "u1 line 9: $(sed -n '9p' "$FILE_U1")"
echo "u2 line 9: $(sed -n '9p' "$FILE_U2")"

# Cleanup
kill $U1_PID $U2_PID 2>/dev/null
sleep 0.5

echo ""
echo "=== Checking u1 log for send errors ==="
grep -E "sent msg|send failed|serialize" log_files/u1.log | tail -20

echo ""
echo "=== Checking u2 log for received messages ==="
grep -E "Received update|line_no" log_files/u2.log | tail -20
