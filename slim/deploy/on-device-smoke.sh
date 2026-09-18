#!/bin/sh
# On-device smoke test for the T830 (runs ON the CPE, via adb shell).
#
# Exercises everything that does not need a model endpoint: the slash commands, the
# session store, the change queue and its dry-run, and the fail-closed tool policy.
# Deliberately does not delete anything: every run writes into its own directory.
#
# Usage: sh on-device-smoke.sh [basedir]
set -u

DIR=${1:-/tmp/slim-smoke}
DATA="$DIR/data-$$"
BIN="$DIR/slim-agent-t830-static"

echo "== environment =="
uname -a
cat /etc/os-release 2>/dev/null | head -3
echo "kernel: $(cat /proc/version 2>/dev/null | head -c 120)"
echo "python3: $(python3 -V 2>&1 || echo none)"
echo "sh: $(readlink -f /bin/sh 2>/dev/null || echo '?')"
echo "-- memory --"
free -m 2>/dev/null | head -3
cat /proc/meminfo 2>/dev/null | grep -E 'MemTotal|MemAvailable' | sed 's/^/  /'
echo "-- storage --"
df -h / /tmp /overlay /data 2>/dev/null | head -6
echo "-- musl runtime present? --"
ls -l /lib/ld-musl-* 2>/dev/null || echo "  no musl loader (the static build does not need one)"

echo "== binary =="
ls -l "$BIN" || { echo "FAIL: $BIN missing"; exit 1; }
"$BIN" --help 2>&1 | head -12

echo "== no-model smoke: slash commands =="
mkdir -p "$DATA" || { echo "FAIL: cannot create $DATA"; exit 1; }
"$BIN" --data-dir "$DATA" --session smoke --once "/help"
echo "--- write and read back ---"
"$BIN" --data-dir "$DATA" --session smoke --once "/write note.txt hello-from-t830"
echo "write rc=$?"
echo "file: $(cat "$DATA/note.txt" 2>/dev/null)"

echo "== session store =="
"$BIN" --data-dir "$DATA" --session smoke --once "/history"
echo "--- file shape ---"
head -3 "$DATA/sessions/smoke.jsonl" 2>/dev/null
echo "mode: $(ls -l "$DATA/sessions/smoke.jsonl" 2>/dev/null | awk '{print $1}')"

echo "== change queue: dry run must write nothing =="
"$BIN" --data-dir "$DATA" --session smoke --dry-run-writes --once "/write nope.txt should-not-exist"
if [ -f "$DATA/nope.txt" ]; then echo "FAIL: dry-run wrote the file"; else echo "ok: dry-run wrote nothing"; fi

echo "== fail-closed policy =="
echo "--- model/human write into the session store must be refused ---"
"$BIN" --data-dir "$DATA" --session smoke --allow-write --once "/write sessions/smoke.jsonl pwned"
if grep -q pwned "$DATA/sessions/smoke.jsonl" 2>/dev/null; then echo "FAIL: the session store was overwritten"; else echo "ok: the session store is intact"; fi
echo "--- the state database must be refused ---"
"$BIN" --data-dir "$DATA" --session smoke --allow-write --once "/write slim.sqlite pwned"
"$BIN" --data-dir "$DATA" --session smoke --once "/write ../escape.txt pwned"

echo "== model reachable? (optional, no failure if not) =="
if [ -n "${SLIM_BASE_URL:-}" ]; then
  "$BIN" --data-dir "$DATA" --session smoke --base-url "$SLIM_BASE_URL" --model "${SLIM_MODEL:-m}" \
    --stream --http-verbose --once "reply with one word: pong" 2>&1 | head -6
else
  echo "  SLIM_BASE_URL not set, skipping the live model check"
fi

echo "== result =="
ls -la "$DATA" 2>/dev/null
echo "== done (data dir kept at $DATA) =="
