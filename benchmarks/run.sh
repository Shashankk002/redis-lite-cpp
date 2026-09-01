#!/usr/bin/env bash
#
# Starts a fresh redis-lite-server, runs the benchmark against it, stops it.
# Any extra arguments are passed through to redis-lite-bench.
#
#   ./benchmarks/run.sh --ops 20000 --pipeline 64

set -u

PORT="${PORT:-6379}"
AOF="${AOF:-/tmp/redis-lite-bench.aof}"
SERVER_LOG="${SERVER_LOG:-/tmp/redis-lite-bench.log}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-build}"

SERVER="$ROOT/$BUILD/redis-lite-server"
BENCH="$ROOT/$BUILD/redis-lite-bench"

if [ ! -x "$SERVER" ] || [ ! -x "$BENCH" ]; then
    echo "build first:  cmake --build $BUILD"
    exit 1
fi

rm -f "$AOF"
"$SERVER" "$AOF" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill -INT "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null' EXIT
sleep 0.6

"$BENCH" --port "$PORT" "$@"

echo
echo "server log: $(wc -l < "$SERVER_LOG" | tr -d ' ') lines, $(wc -c < "$SERVER_LOG" | tr -d ' ') bytes"
echo "aof:        $(wc -c < "$AOF" | tr -d ' ') bytes"
