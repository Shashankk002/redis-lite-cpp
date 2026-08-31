# Redis-Lite

A Redis-inspired in-memory key-value database written from scratch in C++17.

This is a learning / systems-programming project. The goal is not to replace Redis, but to
understand — by building them — the pieces that make a server like Redis work: networking,
protocol parsing, hash tables, expiration, and persistence. Everything is written against
the standard library and the OS; third-party dependencies are avoided on purpose.

Each stage introduces only the concepts that stage needs. The code is meant to stay readable
for someone with intermediate C++ knowledge, so it favours straightforward implementations
over abstractions that might pay off later.

## Status

**Stages 0 and 1 complete. No Redis functionality exists yet.**

What exists today:

- A CMake build (C++17, warnings enabled) producing four executables.
- `redis-lite-server`, which prints a startup line and exits. **It does not listen on a
  socket.** The Stage 1 networking code is not wired into it.
- `redis-lite-tests`, a plain test executable wired into CTest.
- `tcp-echo-server` and `tcp-echo-client` in `experiments/` — a standalone exercise in the
  TCP socket lifecycle, kept separate from the server on purpose (see below).

What does **not** exist yet: a server that actually accepts connections, concurrency of any
kind, the RESP protocol, commands, key-value storage, TTL/expiration, and persistence. Every
stage below marked *planned* describes intent, not shipped code.

## Stage 1 experiment: TCP echo

`experiments/` holds a two-program exercise in the plain POSIX socket API, written to
understand the connection lifecycle before any of it goes into the real server. The server
is blocking and single-shot: it handles exactly one client, echoes one message, and exits.
No event loop, no threads, no non-blocking I/O.

The full lifecycle it walks through: `socket` → `bind` → `listen` → `accept` → `recv` →
`send` → `close`.

Run it in two terminals, or use the one-liner below:

```bash
# terminal 1
./build/tcp-echo-server

# terminal 2
./build/tcp-echo-client hello
```

The client is optional — netcat speaks the same (non-)protocol:

```bash
printf 'hello' | nc localhost 6380
```

It listens on **127.0.0.1:6380**: loopback only, so nothing off this machine can reach it,
and port 6380 rather than 6379 so it never collides with a real Redis.

## Planned stages

| Stage | Focus | Status |
| ----- | ----- | ------ |
| 0 | Project foundation: build system, layout, test executable | **Done** |
| 1 | TCP server: listen, accept, echo bytes back (standalone experiment) | **Done** |
| 2 | RESP protocol: parse requests, serialize replies | Planned |
| 3 | Core commands: `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS` | Planned |
| 4 | Key-value store: the hash table behind the commands | Planned |
| 5 | Expiration: `EXPIRE`, `TTL`, lazy and active eviction | Planned |
| 6 | Event loop: non-blocking I/O, many concurrent clients | Planned |
| 7 | Richer data types: lists, hashes, sets | Planned |
| 8 | Persistence: snapshotting, and an append-only log | Planned |
| 9 | Benchmarks and tuning | Planned |

Stage boundaries may shift; the table records the intended order.

## Layout

```
├── CMakeLists.txt   the whole build
├── src/             server sources
├── include/         shared headers (empty until a header is needed)
├── tests/           test executable, run via CTest
├── experiments/     standalone learning exercises, not part of the server
└── benchmarks/      reserved for a later stage; empty
```

## Build, run, test

```bash
cmake -S . -B build
cmake --build build
./build/redis-lite-server
ctest --test-dir build --output-on-failure
```

The Stage 1 experiment builds alongside the rest; see the section above for running it.
