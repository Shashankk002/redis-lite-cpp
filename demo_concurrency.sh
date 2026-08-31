#!/usr/bin/env bash
#
# Demonstrates that redis-lite-server handles clients concurrently.
#
# Starts its own server, runs a few clients against it, and shuts everything
# down again. Usage:  ./demo_concurrency.sh

set -u

HOST=127.0.0.1
PORT="${PORT:-6379}"
SERVER=./build/redis-lite-server
LOG=/tmp/redis-lite-demo.log

if [ ! -x "$SERVER" ]; then
    echo "build the project first:  cmake --build build"
    exit 1
fi

# Sends one RESP request and prints the reply. ^M marks the CR of each CRLF.
send() {
    printf "$1" | nc -w2 "$HOST" "$PORT" | cat -v
}

SLOW_PID=""
cleanup() {
    # Disconnect clients first: the server joins its client threads on the way
    # out, so it waits for connected clients to go away.
    [ -n "$SLOW_PID" ] && kill "$SLOW_PID" 2>/dev/null
    sleep 0.3
    kill -INT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null

    # Printed only now: the server's stdout is block-buffered into the log
    # file, so nothing is flushed until it exits.
    echo
    echo "server log:"
    sed 's/^/  /' "$LOG"
}

echo "starting $SERVER on $HOST:$PORT"
"$SERVER" > "$LOG" 2>&1 &
SERVER_PID=$!
trap cleanup EXIT
sleep 0.5
printf '*1\r\n$4\r\nPING\r\n' | nc -w2 "$HOST" "$PORT" > /dev/null 2>&1   # warm-up

echo
echo "=== 1. a slow client does not block anybody else ==="
# Client A connects and then says nothing at all for 12 seconds.
sleep 12 | nc "$HOST" "$PORT" > /dev/null 2>&1 &
SLOW_PID=$!
sleep 0.5
echo "client A connected (pid $SLOW_PID) and is sending nothing"

for i in 1 2 3; do
    printf "  %s  client B: PING -> " "$(date +%T)"
    send '*1\r\n$4\r\nPING\r\n'
done

if kill -0 "$SLOW_PID" 2>/dev/null; then
    echo "  client A is STILL connected -- B was served regardless"
else
    echo "  client A already finished (demo timing too tight)"
fi

echo
echo "=== 2. clients share the store ==="
printf "  client A: SET foo bar -> "
send '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n'
printf "  client B: GET foo     -> "
send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'

echo
echo "=== 3. two clients at once, on different keys ==="
( send '*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n' \
      > /tmp/redis-lite-client-c.out ) &
C_PID=$!
( send '*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n*2\r\n$3\r\nGET\r\n$2\r\nk2\r\n' \
      > /tmp/redis-lite-client-d.out ) &
D_PID=$!

# Wait for these two only -- a bare `wait` would also wait out the slow client.
wait "$C_PID" "$D_PID"

echo "  client C: SET k1 v1 / GET k1 -> $(tr -d '\n' < /tmp/redis-lite-client-c.out)"
echo "  client D: SET k2 v2 / GET k2 -> $(tr -d '\n' < /tmp/redis-lite-client-d.out)"

echo
echo "=== 4. TTL still works with other clients connected ==="
printf "  client E: SET t v / EXPIRE t 2 / TTL t -> "
send '*3\r\n$3\r\nSET\r\n$1\r\nt\r\n$1\r\nv\r\n*3\r\n$6\r\nEXPIRE\r\n$1\r\nt\r\n$1\r\n2\r\n*2\r\n$3\r\nTTL\r\n$1\r\nt\r\n'
echo "  waiting 3 seconds..."
sleep 3
printf "  client F: GET t / TTL t -> "
send '*2\r\n$3\r\nGET\r\n$1\r\nt\r\n*2\r\n$3\r\nTTL\r\n$1\r\nt\r\n'

echo
echo "=== 5. the server is still healthy ==="
printf "  client G: PING / GET foo -> "
send '*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'
