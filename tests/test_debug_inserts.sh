#!/bin/bash
BIN="./build/editor"
pkill -9 -f "$BIN" 2>/dev/null
rm -f user_docs/u1_doc.txt user_docs/u2_doc.txt *.log
sleep 0.5

echo "int x = 10;" > user_docs/u1_doc.txt
echo "int x = 10;" > user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=0 SYNCTEXT_MERGE_WRITE_GRACE_MS=100 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u1 > log_files/u1.log 2>&1 &
U1=$!
sleep 0.3

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=0 SYNCTEXT_DEBUG_LEVEL=2 SYNCTEXT_MERGE_WRITE_GRACE_MS=100 SYNCTEXT_STARTUP_SYNC=0 "$BIN" u2 > log_files/u2.log 2>&1 &
U2=$!
sleep 1

cat > user_docs/u1_doc.txt << 'EOF'
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

echo "Pasted content, waiting 2 seconds..."
sleep 2

U1_LINES=$(wc -l < user_docs/u1_doc.txt)
U2_LINES=$(wc -l < user_docs/u2_doc.txt)
echo "u1: $U1_LINES lines"
echo "u2: $U2_LINES lines"

echo ""
echo "=== u2 debug log (insert application) ==="
grep -A 1 "Applying.*inserts" log_files/u2.log | head -30

kill $U1 $U2 2>/dev/null
