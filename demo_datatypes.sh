#!/usr/bin/env bash
#
# Demonstrates the Stage 9 data types over a real TCP connection:
# lists, hashes, sets, WRONGTYPE errors, TTL on a collection, and persistence.
#
# Usage:  ./demo_datatypes.sh

set -u

HOST=127.0.0.1
PORT="${PORT:-6379}"
SERVER=./build/redis-lite-server
AOF=/tmp/redis-lite-types.aof
LOG=/tmp/redis-lite-types.log

if [ ! -x "$SERVER" ]; then
    echo "build the project first:  cmake --build build"
    exit 1
fi

resp() {
    printf '*%d\r\n' "$#"
    for argument in "$@"; do
        printf '$%d\r\n' "${#argument}"
        printf '%s\r\n' "$argument"
    done
}

SERVER_PID=""

start_server() {
    "$SERVER" "$AOF" > "$LOG" 2>&1 &
    SERVER_PID=$!
    sleep 0.6
    resp PING | nc -w2 "$HOST" "$PORT" > /dev/null 2>&1   # warm-up
}

stop_server() {
    [ -n "$SERVER_PID" ] && kill -INT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

# Runs one command and prints "  CMD ARGS -> reply".
show() {
    printf '  %-34s -> ' "$*"
    resp "$@" | nc -w2 "$HOST" "$PORT" | cat -v | tr '\n' ' '
    echo
}

trap 'stop_server' EXIT

rm -f "$AOF"
start_server
echo "server: $(grep loaded "$LOG" | tail -1)"

echo
echo "=== 1. lists ==="
show RPUSH fruits banana
show RPUSH fruits cherry
show LPUSH fruits apple
show LLEN fruits
show LRANGE fruits 0 -1
show LRANGE fruits 1 1
show LRANGE fruits -2 -1
show LPOP fruits
show RPOP fruits
show LRANGE fruits 0 -1

echo
echo "=== 2. hashes ==="
show HSET user name Shashank
show HSET user city Hyderabad
show HSET user name Shash
show HGET user name
show HGET user missing
show HLEN user
show HEXISTS user city
show HDEL user city
show HEXISTS user city
show HLEN user

echo
echo "=== 3. sets ==="
show SADD colours red
show SADD colours green
show SADD colours red
show SCARD colours
show SISMEMBER colours green
show SISMEMBER colours blue
show SMEMBERS colours
show SREM colours red
show SCARD colours

echo
echo "=== 4. WRONGTYPE: one key holds exactly one type ==="
show SET plain "just a string"
show LPUSH plain x
show HSET plain f v
show SADD plain m
show GET fruits
show LLEN user
show HGET colours f
echo "  (the string is unharmed:)"
show GET plain

echo
echo "=== 5. TTL on a non-string value ==="
show RPUSH short a
show RPUSH short b
show EXPIRE short 2
show TTL short
show LLEN short
echo "  --- waiting 3 seconds ---"
sleep 3
show LLEN short
show TTL short
show LRANGE short 0 -1

echo
echo "=== 6. persistence across a restart ==="
show RPUSH saved-list one
show RPUSH saved-list two
show HSET saved-hash field value
show SADD saved-set member
show EXPIRE saved-list 600
echo "  --- stopping and restarting the server ---"
stop_server
start_server
echo "  server: $(grep loaded "$LOG" | tail -1)"
show LRANGE saved-list 0 -1
show LLEN saved-list
show TTL saved-list
show HGET saved-hash field
show SMEMBERS saved-set
show SCARD saved-set

echo
echo "=== 7. the append-only log for the new types ==="
grep -aE '^(LPUSH|RPUSH|LPOP|RPOP|HSET|HDEL|SADD|SREM)' "$AOF" | head -20 | sed 's/^/    /'
echo "    ($(wc -l < "$AOF" | tr -d ' ') records, $(wc -c < "$AOF" | tr -d ' ') bytes total)"
