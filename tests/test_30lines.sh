#!/usr/bin/env bash
# Test 30 lines to check if 9, 19, 29 fail

BIN="./build/editor"
cleanup() {
    pkill -9 -f "$BIN" 2>/dev/null || true
    rm -f /dev/mqueue/synctext_user_* /dev/shm/synctext_registry 2>/dev/null || true
}

cleanup
sleep 1

echo "Line 0" > user_docs/u1_doc.txt
cp user_docs/u1_doc.txt user_docs/u2_doc.txt

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=2 "$BIN" u1 > log_files/u1.log 2>&1 &
sleep 1
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STARTUP_SYNC=0 SYNCTEXT_DEBUG_LEVEL=2 "$BIN" u2 > log_files/u2.log 2>&1 &
sleep 2

# Add 30 lines
{
    echo "Line 0"
    for i in {1..30}; do echo "Line $i"; done
} > user_docs/u1_doc.txt

sleep 3

echo "=== Lines sent by u1 ==="
grep "sent msg" log_files/u1.log | grep "type=3" | grep -oE "line=[0-9]+" | sort -t= -k2 -n

echo ""
echo "=== Missing lines ==="
for i in {1..30}; do
    if ! grep -q "sent msg.*line=$i.*type=3" log_files/u1.log; then
        echo "Line $i NOT SENT"
    fi
done

echo ""
echo "=== u2 result ==="
wc -l < user_docs/u2_doc.txt
echo "Lines in u2:"
cat user_docs/u2_doc.txt | grep -E "Line (9|19|29)"

cleanup
