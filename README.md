# Redis-Lite

A Redis-inspired in-memory key-value database written from scratch in modern C++ (C++17).

This is a learning / systems-programming project. The goal is not to replace Redis, but to
understand — by building them — the pieces that make a server like Redis work: event-driven
networking, protocol parsing, hash tables, expiration, and persistence. Everything is
implemented directly against the standard library and the OS; third-party dependencies are
avoided on purpose.

## Status

**Stage 0 (project foundation) — complete. No Redis functionality exists yet.**

What exists today:

- A CMake build (C++17, warnings enabled) producing one executable, `redis-lite-server`.
- That executable prints a startup line and exits. It does not listen on a socket.
- A dependency-free test harness wired into CTest, with smoke tests that prove it runs.

What does **not** exist yet: networking of any kind (TCP, sockets, epoll/kqueue), threading,
the RESP protocol, commands, key-value storage, TTL/expiration, and persistence. Every item
below marked *planned* is a description of intent, not of shipped code.

## Planned stages

| Stage | Focus | Status |
| ----- | ----- | ------ |
| 0 | Project foundation: build system, layout, test harness | **Done** |
| 1 | TCP server: listen, accept, echo bytes back | Planned |
| 2 | RESP protocol: parse requests, serialize replies | Planned |
| 3 | Core commands: `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS` | Planned |
| 4 | Key-value store: the hash table behind the commands | Planned |
| 5 | Expiration: `EXPIRE`, `TTL`, lazy and active eviction | Planned |
| 6 | Event loop: non-blocking I/O, many concurrent clients | Planned |
| 7 | Richer data types: lists, hashes, sets | Planned |
| 8 | Persistence: snapshotting, and an append-only log | Planned |
| 9 | Benchmarks and tuning | Planned |

Stage boundaries may shift as the project goes on; the table records the intended order.

## Layout

```
├── CMakeLists.txt      top-level build
├── include/redis_lite/ public headers
├── src/                server sources and the executable target
├── tests/              test harness and test cases (run via CTest)
└── benchmarks/         reserved for later stages; no targets yet
```

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/src/redis-lite-server
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Build options

| Option | Default | Effect |
| ------ | ------- | ------ |
| `REDIS_LITE_BUILD_TESTS` | `ON` | Build the test suite |
| `REDIS_LITE_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` / `/WX` |
