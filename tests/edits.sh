#!/usr/bin/env bash
# Apply deterministic edits to existing user docs only (no launching editors).
# Usage: ./edits.sh [u1 u2 u3 u4]
# Env:
#   ROUNDS=<n>     default 1
#   SLEEP_MS=<ms>  default 1000
set -euo pipefail

ROUNDS=${ROUNDS:-1}
SLEEP_MS=${SLEEP_MS:-3000}

if [ $# -gt 0 ]; then
  USERS=("$@")
else
  USERS=(u1 u2 u3 u4)
fi

msleep() { python3 -c 'import time,sys; time.sleep(float(sys.argv[1])/1000.0)' "$1" 2>/dev/null || sleep 1; }

doc_file() { echo "$1"_doc.txt; }
append_line() { echo "$2" >>"$1"; }
insert_top() {
  local f="$1"; local txt="$2"
  if [ ! -f "$f" ]; then echo "$txt" >"$f"; return; fi
  printf '%s\n%s\n' "$txt" "$(cat "$f")" >"$f.tmp" && mv "$f.tmp" "$f"
}

delete_last() {
  local f="$1"
  [ -f "$f" ] || return 0
  local n; n=$(wc -l <"$f" 2>/dev/null || echo 0)
  if [ "$n" -le 1 ]; then : >"$f"; else sed -i "${n}d" "$f"; fi
}

for r in $(seq 1 "$ROUNDS"); do
  echo "[edits] Round $r"
  tstamp=$(date +%s)
  # u1 append
  f1=$(doc_file "${USERS[0]}"); append_line "$f1" "[edits $tstamp] append from ${USERS[0]}"; msleep "$SLEEP_MS"
  # u2 insert top
  if [ ${#USERS[@]} -ge 2 ]; then f2=$(doc_file "${USERS[1]}"); insert_top "$f2" "[edits $tstamp] insert-top from ${USERS[1]}"; msleep "$SLEEP_MS"; fi
  # u3 delete last
  if [ ${#USERS[@]} -ge 3 ]; then f3=$(doc_file "${USERS[2]}"); delete_last "$f3"; msleep "$SLEEP_MS"; fi
  # u4 two-line append
  if [ ${#USERS[@]} -ge 4 ]; then f4=$(doc_file "${USERS[3]}"); append_line "$f4" "[edits $tstamp] block-1 from ${USERS[3]}"; append_line "$f4" "[edits $tstamp] block-2 from ${USERS[3]}"; msleep "$SLEEP_MS"; fi
 done

  # Final stabilization pause
  msleep 1000
  echo "[edits] Done. Review u*_doc.txt in this folder and in running editors."
