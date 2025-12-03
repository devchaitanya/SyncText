#!/usr/bin/env bash
set -euo pipefail

USER_A=u1
USER_B=u2
BIN=./build/editor

# Clean state
pkill -9 -f "./ build/editor" 2>/dev/null || true
rm -f user_docs/u1_doc.txt user_docs/u2_doc.txt *.log
sleep 0.5

# Prepare baseline
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u1_doc.txt
printf "int x = 10;\nint y = 20;\nint z = 30;\n" > user_docs/u2_doc.txt

echo "[test] Baseline prepared:"
cat user_docs/u1_doc.txt

# Launch editors
SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 \
SYNCTEXT_MERGE_WRITE_GRACE_MS=200 SYNCTEXT_STARTUP_SYNC=0 \
  "$BIN" "$USER_A" >u1.log 2>&1 &
echo "[test] Launched u1 (pid $!)"
sleep 0.5

SYNCTEXT_NO_AUTO_OPEN=1 SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2 \
SYNCTEXT_MERGE_WRITE_GRACE_MS=200 SYNCTEXT_STARTUP_SYNC=0 \
  "$BIN" "$USER_B" >u2.log 2>&1 &
echo "[test] Launched u2 (pid $!)"
sleep 0.8

# u1 changes line 1
echo "[test] u1 changes x=10 to x=50"
sed -i '1s@.*@int x = 50;@' user_docs/u1_doc.txt
sleep 0.5

# u2 changes line 2
echo "[test] u2 changes y=20 to y=100"
sed -i '2s@.*@int y = 100;@' user_docs/u2_doc.txt
sleep 3

echo "=== Final state ==="
echo "--- user_docs/u1_doc.txt ---"
cat user_docs/u1_doc.txt
echo "--- user_docs/u2_doc.txt ---"
cat user_docs/u2_doc.txt

# Check
if grep -q "int y = 100;" user_docs/u1_doc.txt && grep -q "int x = 50;" user_docs/u2_doc.txt; then
  echo "[PASS] Both files converged"
else
  echo "[FAIL] Files did not converge"
  echo "=== u1 log tail ===" 
  tail -30 log_files/u1.log
  echo "=== u2 log tail ==="
  tail -30 log_files/u2.log
  exit 1
fi
