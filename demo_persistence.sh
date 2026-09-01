#!/usr/bin/env bash
#
# Demonstrates that Redis-Lite data survives a restart, using the append-only log.
#
# Usage:  ./demo_persistence.sh

set -u

HOST=127.0.0.1
PORT="${PORT:-6379}"
SERVER=./build/redis-lite-server
AOF=/tmp/redis-lite-demo.aof
LOG=/tmp/redis-lite-demo.log

if [ ! -x "$SERVER" ]; then
    echo "build the project first:  cmake --build build"
    exit 1
fi

SERVER_PID=""

send() {
    printf "$1" | nc -w2 "$HOST" "$PORT" | cat -v | tr -d '\n'
}

start_server() {
    "$SERVER" "$AOF" > "$LOG" 2>&1 &
    SERVER_PID=$!
    sleep 0.6
    printf '*1\r\n$4\r\nPING\r\n' | nc -w2 "$HOST" "$PORT" > /dev/null 2>&1   # warm-up
}

stop_server() {
    [ -n "$SERVER_PID" ] && kill -INT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

restart() {
    echo "  --- stopping the server ---"
    stop_server
    echo "  --- starting it again ---"
    start_server
    echo "  server says: $(grep 'loaded' "$LOG" | tail -1)"
}

trap 'stop_server' EXIT

rm -f "$AOF"
echo "starting with no log file at all ($AOF removed)"
start_server
echo "server says: $(grep 'loaded' "$LOG" | tail -1)"

echo
echo "=== 1. a plain SET survives a restart ==="
echo "  SET foo bar     -> $(send '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n')"
echo "  SET count 42    -> $(send '*3\r\n$3\r\nSET\r\n$5\r\ncount\r\n$2\r\n42\r\n')"
restart
echo "  GET foo         -> $(send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n')"
echo "  GET count       -> $(send '*2\r\n$3\r\nGET\r\n$5\r\ncount\r\n')"

echo
echo "=== 2. the persistence file, as written so far ==="
sed 's/^/    /' "$AOF"
echo "    (SET <keylen> <key> <valuelen> <value>)"

echo
echo "=== 3. a DEL survives a restart ==="
echo "  DEL foo         -> $(send '*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n')"
restart
echo "  GET foo         -> $(send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n')   (nil expected)"

echo
echo "=== 4. an EXPIRE survives a restart, and keeps counting down ==="
echo "  SET temp value  -> $(send '*3\r\n$3\r\nSET\r\n$4\r\ntemp\r\n$5\r\nvalue\r\n')"
echo "  EXPIRE temp 20  -> $(send '*3\r\n$6\r\nEXPIRE\r\n$4\r\ntemp\r\n$2\r\n20\r\n')"
echo "  TTL temp        -> $(send '*2\r\n$3\r\nTTL\r\n$4\r\ntemp\r\n')"
echo "  --- waiting 3 seconds so the countdown is visible ---"
sleep 3
restart
echo "  GET temp        -> $(send '*2\r\n$3\r\nGET\r\n$4\r\ntemp\r\n')   (still alive)"
echo "  TTL temp        -> $(send '*2\r\n$3\r\nTTL\r\n$4\r\ntemp\r\n')   (lower than 20)"

echo
echo "=== 5. a key whose deadline passes while the server is down ==="
echo "  SET brief x     -> $(send '*3\r\n$3\r\nSET\r\n$5\r\nbrief\r\n$1\r\nx\r\n')"
echo "  EXPIRE brief 3  -> $(send '*3\r\n$6\r\nEXPIRE\r\n$5\r\nbrief\r\n$1\r\n3\r\n')"
echo "  --- stopping, waiting 5 seconds, restarting ---"
stop_server
sleep 5
start_server
echo "  server says: $(grep 'loaded' "$LOG" | tail -1)"
echo "  GET brief       -> $(send '*2\r\n$3\r\nGET\r\n$5\r\nbrief\r\n')   (nil expected)"
echo "  TTL brief       -> $(send '*2\r\n$3\r\nTTL\r\n$5\r\nbrief\r\n')   (-2 expected)"

echo
echo "=== 6. SET clears an old TTL, across a restart ==="
echo "  SET foo bar     -> $(send '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n')"
echo "  EXPIRE foo 100  -> $(send '*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$3\r\n100\r\n')"
echo "  SET foo new     -> $(send '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nnew\r\n')"
restart
echo "  TTL foo         -> $(send '*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n')   (-1 expected)"
echo "  GET foo         -> $(send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n')"

echo
echo "=== 7. replay does not append to the log ==="
BEFORE=$(wc -c < "$AOF" | tr -d ' ')
restart
AFTER=$(wc -c < "$AOF" | tr -d ' ')
echo "  log size before restart: $BEFORE bytes"
echo "  log size after restart:  $AFTER bytes"
if [ "$BEFORE" = "$AFTER" ]; then
    echo "  unchanged -- replay did not write anything back"
fi

echo
echo "=== 8. a key and value containing spaces ==="
echo "  SET 'a key' 'a value'  -> $(send '*3\r\n$3\r\nSET\r\n$5\r\na key\r\n$7\r\na value\r\n')"
restart
echo "  GET 'a key'            -> $(send '*2\r\n$3\r\nGET\r\n$5\r\na key\r\n')"
echo "  the record on disk:"
grep -a 'a key' "$AOF" | tail -1 | sed 's/^/    /'

echo
echo "=== 9. a corrupted log file is refused ==="
stop_server
cp "$AOF" "$AOF.bak"
printf 'THIS IS NOT A VALID RECORD\n' >> "$AOF"
echo "  appended garbage to the log; trying to start:"
"$SERVER" "$AOF" > /tmp/redis-lite-corrupt.out 2>&1
CORRUPT_RC=$?
sed 's/^/    /' /tmp/redis-lite-corrupt.out
echo "  exit code: $CORRUPT_RC  (non-zero means it refused to start)"
mv "$AOF.bak" "$AOF"
echo "  restored the good log; starting again:"
start_server
echo "  server says: $(grep 'loaded' "$LOG" | tail -1)"
echo "  GET foo         -> $(send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n')"

echo
echo "=== 10. the whole log at the end ==="
echo "  $(wc -l < "$AOF" | tr -d ' ') records, $(wc -c < "$AOF" | tr -d ' ') bytes"
