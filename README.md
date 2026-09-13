# Redis-Lite

A Redis-inspired in-memory key-value server built from scratch in C++17.

The project focuses on the core systems behind a database server: TCP networking,
protocol parsing, event-driven I/O, typed in-memory storage, key expiration,
persistence, testing, and performance measurement.

> Built as a learning project to understand how the pieces of a real networked
> database fit together — not as a Redis replacement.

## Highlights

- **C++17** with CMake and no third-party dependencies
- **TCP server** built directly on POSIX sockets
- **RESP2** parser and serializer with incremental/binary-safe parsing
- **Single-threaded `kqueue` event loop** supporting multiple concurrent clients
- **Typed keyspace** using `std::variant`
- **TTL expiration** with lazy eviction
- **Append-only persistence (AOF)** with replay and corrupt-log detection
- **Pipelining and partial I/O** through per-client input/output buffers
- **494 unit checks** plus an integration test that drives the real server
  over TCP
- **Benchmark harness** for sequential and pipelined workloads

## Architecture

```text
                    +----------------------+
                    |    kqueue event      |
                    |       loop           |
                    +----------+-----------+
                               |
                +--------------+--------------+
                |                             |
         listening socket               client sockets
                |                             |
             accept()                   recv() / send()
                                              |
                                      per-client buffers
                                              |
                                           RESP2
                                              |
                                           parse()
                                              |
                                      execute_command()
                                              |
                              +---------------+---------------+
                              |                               |
                         typed Store                     AOF log
                       std::variant                     persistence
                              |                               |
                              +---------------+---------------+
                                              |
                                          serialize()
                                              |
                                       output buffer
                                              |
                                            send()
```

A request follows:

```text
TCP bytes
   ↓
input buffer
   ↓
RESP parser
   ↓
RespValue
   ↓
command execution
   ↓
Store mutation + AOF record
   ↓
RespValue reply
   ↓
RESP serializer
   ↓
output buffer
   ↓
TCP bytes
```

The input and output buffers are necessary because TCP is a byte stream:
one request may arrive across multiple reads, one read may contain multiple
requests, and a large response may require multiple writes.

## Supported Commands

### Strings

| Command | Description |
| --- | --- |
| `PING` | Connection health check |
| `SET` | Store a string value |
| `GET` | Retrieve a string value |
| `DEL` | Delete a key |

### Expiration

| Command | Description |
| --- | --- |
| `EXPIRE` | Set a key expiration time |
| `TTL` | Query remaining lifetime |

### Lists

| Command | Description |
| --- | --- |
| `LPUSH` | Push to the left |
| `RPUSH` | Push to the right |
| `LPOP` | Remove from the left |
| `RPOP` | Remove from the right |
| `LLEN` | Get list length |
| `LRANGE` | Read a range |

### Hashes

| Command | Description |
| --- | --- |
| `HSET` | Set a field |
| `HGET` | Get a field |
| `HDEL` | Delete a field |
| `HEXISTS` | Check for a field |
| `HLEN` | Get field count |

### Sets

| Command | Description |
| --- | --- |
| `SADD` | Add a member |
| `SREM` | Remove a member |
| `SISMEMBER` | Test membership |
| `SCARD` | Get set size |
| `SMEMBERS` | Return all members |

Commands return RESP2-compatible replies and Redis-style WRONGTYPE errors
where appropriate.

## Technical Highlights

### Event-driven networking

The server uses non-blocking POSIX sockets and `kqueue` rather than one
thread per client.

The event loop sleeps until the kernel reports a socket is ready, then drains
available input or output before returning to the event loop.

This allows many connections to share a single execution thread without
requiring a mutex around the database.

### Incremental RESP2 parsing

The parser distinguishes between:

- **Complete** requests
- **Incomplete** requests that require more bytes
- **Malformed** requests

This allows requests to be safely parsed even when TCP splits them across
multiple `recv()` calls.

Pipelined requests are handled by repeatedly parsing complete values from the
same input buffer.

Two limits protect the server from hostile input: arrays may nest at most 128
levels deep (deeper input is rejected as malformed rather than exhausting the
stack), and a client may have at most 16 MiB of an unfinished request buffered
before it is disconnected.

### Typed keyspace

Each key stores exactly one of:

```cpp
std::string
std::deque<std::string>
std::unordered_map<std::string, std::string>
std::unordered_set<std::string>
```

represented using:

```cpp
std::variant
```

This makes the key's type explicit and naturally supports Redis-style
WRONGTYPE semantics.

### Expiration

Keys use lazy expiration with `std::chrono::steady_clock`.

Expiration is stored as an absolute monotonic deadline in memory. Persistent
expiration records use Unix timestamps so they can be reconstructed after a
restart.

### Append-only persistence

Mutating commands are written to a simple length-prefixed append-only log:

```text
SET    <key-length> <key> <value-length> <value>
DEL    <key-length> <key>
EXPIRE <key-length> <key> <unix-deadline>
```

List, hash, and set mutations (`LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `HSET`,
`HDEL`, `SADD`, `SREM`) use the same verb-plus-length-prefixed-arguments
shape. Commands that change nothing, such as a duplicate `SADD`, are not
recorded.

The log is replayed at startup through the same command execution path used by
normal clients, keeping replay semantics consistent with live execution.

Malformed persistence records are rejected rather than partially loading the
database.

The implementation deliberately uses `flush()` rather than `fsync()`, so it
does not claim full crash/power-loss durability.

## Performance

Benchmarks use a persistent TCP connection and real RESP requests.

Two workloads are measured:

- **Sequential:** send one request and wait for its reply
- **Pipelined:** keep multiple requests in flight

Pipelining isolates more of the server's actual throughput from network
round-trip latency.

### Example results

Apple M3, loopback, 16-byte values, pipeline depth 64,
20,000 operations per command.

| Command | Sequential ops/s | Pipelined ops/s |
| --- | ---: | ---: |
| `SET` | 49,837 | 436,894 |
| `GET` | 62,675 | **1,378,831** |
| `DEL` | 54,269 | 485,008 |
| `LPUSH` | 53,475 | 453,328 |
| `LPOP` | 53,769 | 482,005 |
| `HSET` | 52,689 | 422,793 |
| `HGET` | 62,712 | 1,134,465 |
| `SADD` | 52,034 | 447,301 |
| `SISMEMBER` | 62,904 | 1,231,245 |

The measurements also exposed where the time was going:

- Release `-O3` produced roughly **2.5–5×** the throughput of the Debug build.
- Removing per-command console logging improved read throughput by roughly
  **9–12%**.
- Reworking AOF record formatting improved write throughput by roughly
  **5–8%**.
- A log-flush batching optimization measured only ~1.3% improvement and was
  reverted because it was within the benchmark's noise floor.

The important result was not simply a faster number, but identifying the
actual bottlenecks through measurement rather than guessing.

## Testing

The unit suite contains **494 checks** covering:

- RESP parsing and serialization
- Partial and malformed input
- Command behavior and argument validation
- TTL and expiration
- Lists, hashes, and sets
- WRONGTYPE semantics
- AOF persistence and replay
- Corrupt persistence records
- Large requests and responses

A separate integration test (`tests/integration_test.cpp`) starts the real
server binary and exercises it over TCP: ordinary commands, pipelining, a
multi-megabyte request, the request-size limit, and recovery after a
misbehaving client. Both are registered with CTest:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Both Debug and Release builds are verified with zero compiler warnings.

The regression tests for the parser, persistence, and request-limit fixes
were each checked against a deliberately broken build during development to
confirm they fail when the bug is present. That verification was manual; the
repository does not include mutation tooling.

## Build & Run

### Requirements

- macOS or BSD
- C++17 compiler
- CMake

The current server uses `kqueue`, so the networking implementation is
macOS/BSD-specific.

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Start the server

```bash
./build/redis-lite-server
```

The default configuration is:

```text
127.0.0.1:6379
```

A custom persistence file and port can be supplied:

```bash
./build/redis-lite-server /tmp/redis-lite.aof 6390
```

### Try it manually

The server speaks RESP2. For example:

```bash
printf '*1\r\n$4\r\nPING\r\n' | nc -w2 127.0.0.1 6379
```

```bash
printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n' | nc -w2 127.0.0.1 6379
```

```bash
printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n' | nc -w2 127.0.0.1 6379
```

## Demonstrations

Three scripts demonstrate the main networking, persistence, and data-type
features:

```bash
./demo_event_loop.sh
./demo_persistence.sh
./demo_datatypes.sh
```

They start the server, exercise the relevant functionality, assert a
representative set of replies, and clean up after themselves. Each exits
non-zero if an expected reply is wrong.

## Benchmarking

Build a Release version before benchmarking:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

BUILD=build-release ./benchmarks/run.sh --ops 20000 --pipeline 64
```

A different port can be used with:

```bash
PORT=6390 ./benchmarks/run.sh --ops 20000 --pipeline 64
```

## Project Structure

```text
redis-lite-cpp/
├── src/
│   ├── main.cpp
│   ├── server.cpp
│   ├── server.hpp
│   ├── resp.cpp
│   ├── resp.hpp
│   ├── commands.cpp
│   ├── commands.hpp
│   ├── persistence.cpp
│   └── persistence.hpp
├── tests/
│   ├── tests.cpp
│   └── integration_test.cpp
├── benchmarks/
│   ├── bench.cpp
│   └── run.sh
├── experiments/
│   ├── tcp_echo_server.cpp
│   └── tcp_echo_client.cpp
├── demo_event_loop.sh
├── demo_persistence.sh
├── demo_datatypes.sh
└── CMakeLists.txt
```

## Engineering Progression

The project was built incrementally rather than starting with the final
architecture:

| Stage | Focus |
| --- | --- |
| 0 | Project foundation and build system |
| 1 | TCP socket fundamentals |
| 2 | TCP server integration |
| 3 | RESP2 protocol |
| 4 | Core commands |
| 5 | TTL and expiration |
| 6 | Concurrent clients with threads and mutexes |
| 7 | Non-blocking I/O and `kqueue` event loop |
| 8 | Append-only persistence |
| 9 | Lists, hashes and sets |
| 10 | Benchmarking and performance tuning |

The transition from Stage 6 to Stage 7 was intentional: the threaded server
first established the concurrency problem and synchronization requirements,
then the event-driven architecture removed the need for shared-state locking
by returning to a single execution thread.

## Limitations

This project intentionally does not attempt to implement all of Redis.

Current limitations include:

- macOS/BSD networking only (`kqueue`)
- Single-threaded command execution
- No AOF rewriting or compaction
- `flush()` rather than `fsync()`
- No replication or clustering
- No transactions or pub/sub
- No sorted sets
- Some Redis commands and advanced data-type operations are not implemented
- Client output buffers have no hard size limit (input is capped at 16 MiB)

These are deliberate boundaries for the project rather than attempts to claim
feature parity with Redis.

## Future Work

Potential extensions include:

- AOF rewriting / compaction
- Active expiration
- More Redis commands and data structures
- Linux `epoll` support
- Stronger crash-consistency guarantees
- CI across supported platforms

---

Built from scratch to learn how networking, protocols, storage, persistence,
and performance engineering fit together in a real server.
