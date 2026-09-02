#!/usr/bin/env bash
#
# Demonstrates the Stage 9 data types over a real TCP connection:
# lists, hashes, sets, WRONGTYPE errors, TTL on a collection, and persistence.
#
# Usage:  ./demo_datatypes.sh

set -eu

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
    resp PING | nc -w2 "$HOST" "$PORT" > /dev/null 2>&1 || true   # warm-up
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
    return 0
}

LAST_REPLY=""
FAILURES=0

# Runs one command, prints "  CMD ARGS -> reply", and keeps it in LAST_REPLY.
show() {
    LAST_REPLY="$(resp "$@" | nc -w2 "$HOST" "$PORT" | cat -v | tr '\n' ' ')"
    printf '  %-34s -> %s\n' "$*" "$LAST_REPLY"
}

# Fails the script if the last reply did not contain what we expect.
assert_reply() {
    if printf '%s' "$LAST_REPLY" | grep -qF -- "$1"; then
        printf '      ok: %s\n' "$2"
    else
        printf '      FAILED: %s\n' "$2"
        printf '        expected to contain: %s\n' "$1"
        printf '        actual:              %s\n' "$LAST_REPLY"
        FAILURES=$((FAILURES + 1))
    fi
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
assert_reply ':3' 'three items were pushed'
show LRANGE fruits 0 -1
assert_reply 'apple' 'LPUSH put apple at the front'
assert_reply 'cherry' 'RPUSH put cherry at the back'
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
assert_reply ':0' 'updating an existing field reports 0'
show HGET user name
assert_reply 'Shash' 'the hash field reads back updated'
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
assert_reply ':0' 'a duplicate SADD reports 0'
show SCARD colours
assert_reply ':2' 'the duplicate did not grow the set'
show SISMEMBER colours green
show SISMEMBER colours blue
show SMEMBERS colours
show SREM colours red
show SCARD colours

echo
echo "=== 4. WRONGTYPE: one key holds exactly one type ==="
show SET plain "just a string"
show LPUSH plain x
assert_reply 'WRONGTYPE' 'a list command on a string is refused'
show HSET plain f v
show SADD plain m
show GET fruits
show LLEN user
show HGET colours f
echo "  (the string is unharmed:)"
show GET plain
assert_reply 'just a string' 'the string value is returned intact'

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
assert_reply ':0' 'the expired list reads as empty'
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
assert_reply 'one' 'the list survived the restart'
show LLEN saved-list
assert_reply ':2' 'with both of its items'
show TTL saved-list
show HGET saved-hash field
assert_reply 'value' 'the hash survived the restart'
show SMEMBERS saved-set
assert_reply 'member' 'the set survived the restart'
show SCARD saved-set

echo
echo "=== 7. the append-only log for the new types ==="
grep -aE '^(LPUSH|RPUSH|LPOP|RPOP|HSET|HDEL|SADD|SREM)' "$AOF" | head -20 | sed 's/^/    /'
echo "    ($(wc -l < "$AOF" | tr -d ' ') records, $(wc -c < "$AOF" | tr -d ' ') bytes total)"

echo
if [ "$FAILURES" -ne 0 ]; then
    echo "$FAILURES assertion(s) failed"
    exit 1
fi
echo "all assertions passed"
