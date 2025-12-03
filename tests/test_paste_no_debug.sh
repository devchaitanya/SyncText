#!/bin/bash
# Test multi-line paste WITHOUT debug logging

BIN="./build/editor"
FILE_U1="user_docs/u1_doc.txt"
FILE_U2="user_docs/u2_doc.txt"

# Cleanup
pkill -9 -f "$BIN" 2>/dev/null
rm -f "$FILE_U1" "$FILE_U2" *.log
sleep 0.5

# Start with empty files
echo "int x = 10;" > "$FILE_U1"
echo "int x = 10;" > "$FILE_U2"

# Start editors WITHOUT debug logging
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1_PID=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
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
    # Check line 9 specifically
    U1_LINE9=$(sed -n '9p' "$FILE_U1")
    U2_LINE9=$(sed -n '9p' "$FILE_U2")
    if [ "$U1_LINE9" = "$U2_LINE9" ]; then
        echo "[PASS] Line 9 matches: '$U1_LINE9'"
    else
        echo "[FAIL] Line 9 differs!"
        echo "  u1: '$U1_LINE9'"
        echo "  u2: '$U2_LINE9'"
    fi
else
    echo "[FAIL] Line counts differ!"
fi

# Cleanup
kill $U1_PID $U2_PID 2>/dev/null
