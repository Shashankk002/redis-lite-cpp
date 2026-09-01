#!/usr/bin/env bash
#
# Demonstrates the Stage 7 kqueue event loop: one thread serving many clients.
#
# Starts its own server, runs clients against it, then shuts it down.
# Usage:  ./demo_event_loop.sh

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

IDLE_PID=""
cleanup() {
    # Disconnect clients first, then Ctrl+C the server.
    [ -n "$IDLE_PID" ] && kill "$IDLE_PID" 2>/dev/null
    sleep 0.3
    kill -INT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null

    # Printed only now: the server's stdout is block-buffered into the log
    # file, so nothing is flushed until it exits.
    echo
    echo "server log (tail):"
    tail -12 "$LOG" | sed 's/^/  /'
}

echo "starting $SERVER on $HOST:$PORT"
"$SERVER" > "$LOG" 2>&1 &
SERVER_PID=$!
trap cleanup EXIT
sleep 0.5
printf '*1\r\n$4\r\nPING\r\n' | nc -w2 "$HOST" "$PORT" > /dev/null 2>&1   # warm-up

echo
echo "=== 1. an idle client does not block anybody else ==="
# Client A connects and then stays completely silent for 12 seconds.
sleep 12 | nc "$HOST" "$PORT" > /dev/null 2>&1 &
IDLE_PID=$!
sleep 0.5
echo "client A connected (pid $IDLE_PID) and is sending nothing at all"

for i in 1 2 3; do
    printf "  %s  client B: PING -> " "$(date +%T)"
    send '*1\r\n$4\r\nPING\r\n'
done

if kill -0 "$IDLE_PID" 2>/dev/null; then
    echo "  client A is STILL connected -- one thread served B anyway"
fi

echo
echo "=== 2. clients share one store ==="
printf "  client A: SET foo bar -> "
send '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n'
printf "  client B: GET foo     -> "
send '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'

echo
echo "=== 3. two clients at the same time, different keys ==="
( send '*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n' \
      > /tmp/redis-lite-client-c.out ) &
C_PID=$!
( send '*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n*2\r\n$3\r\nGET\r\n$2\r\nk2\r\n' \
      > /tmp/redis-lite-client-d.out ) &
D_PID=$!
wait "$C_PID" "$D_PID"
echo "  client C: SET k1 v1 / GET k1 -> $(tr -d '\n' < /tmp/redis-lite-client-c.out)"
echo "  client D: SET k2 v2 / GET k2 -> $(tr -d '\n' < /tmp/redis-lite-client-d.out)"

echo
echo "=== 4. pipelining: PING + SET + GET in ONE write ==="
printf "  -> "
send '*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$3\r\npip\r\n$3\r\nyes\r\n*2\r\n$3\r\nGET\r\n$3\r\npip\r\n' \
    | tr -d '\n'
echo

echo
echo "=== 5. ONE request split across TWO writes, half a second apart ==="
printf "  -> "
{ printf '*2\r\n$3\r\nGET\r\n'; sleep 0.5; printf '$3\r\nfoo\r\n'; } \
    | nc -w2 "$HOST" "$PORT" | cat -v | tr -d '\n'
echo

echo
echo "=== 6. a large reply, which needs several writes ==="
BIG=$(head -c 200000 /dev/zero | tr '\0' 'x')
BYTES=$( { printf '*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$200000\r\n'
           printf '%s' "$BIG"
           printf '\r\n*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n'
         } | nc -w2 "$HOST" "$PORT" | wc -c )
echo "  SET a 200000-byte value, then GET it back"
echo "  bytes received: $(echo "$BYTES" | tr -d ' ')   (expected 200016: +OK + \$200000 header + payload + CRLF)"

echo
echo "=== 7. malformed input does not take the server down ==="
printf "  garbage       -> "
send '!garbage\r\n'
printf "  raw text      -> "
send 'hello world\r\n'
printf "  still alive   -> "
send '*1\r\n$4\r\nPING\r\n'

echo
echo "=== 8. disconnect, then reconnect ==="
printf "  connection 1: SET rc 1 -> "
send '*3\r\n$3\r\nSET\r\n$2\r\nrc\r\n$1\r\n1\r\n'
printf "  connection 2: GET rc   -> "
send '*2\r\n$3\r\nGET\r\n$2\r\nrc\r\n'

echo
echo "=== 9. TTL still works ==="
printf "  SET t v / EXPIRE t 2 / TTL t -> "
send '*3\r\n$3\r\nSET\r\n$1\r\nt\r\n$1\r\nv\r\n*3\r\n$6\r\nEXPIRE\r\n$1\r\nt\r\n$1\r\n2\r\n*2\r\n$3\r\nTTL\r\n$1\r\nt\r\n' \
    | tr -d '\n'
echo
echo "  waiting 3 seconds..."
sleep 3
printf "  GET t / TTL t -> "
send '*2\r\n$3\r\nGET\r\n$1\r\nt\r\n*2\r\n$3\r\nTTL\r\n$1\r\nt\r\n' | tr -d '\n'
echo

echo
echo "=== 10. the server is still healthy ==="
printf "  PING / GET foo -> "
send '*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n' | tr -d '\n'
echo
