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

**Stages 0–2 complete. No Redis functionality exists yet — the server echoes bytes.**

What exists today:

- A CMake build (C++17, warnings enabled) producing four executables.
- `redis-lite-server`, which now listens on **127.0.0.1:6379**, accepts one client at a time,
  echoes back whatever it receives, and keeps accepting new clients until Ctrl+C. It does not
  understand a single Redis command yet.
- `redis-lite-tests`, a plain test executable wired into CTest.
- `tcp-echo-server` and `tcp-echo-client` in `experiments/` — the Stage 1 exercise, kept as-is.

What does **not** exist yet: the RESP protocol, commands, key-value storage, TTL/expiration,
persistence, and any form of concurrency. The server is **sequential**: a second client waits
in the kernel's backlog until the first one disconnects. Every stage below marked *planned*
describes intent, not shipped code.

## Trying the server

```bash
# terminal 1
./build/redis-lite-server
# Redis-Lite server listening on 127.0.0.1:6379

# terminal 2
nc localhost 6379
```

Type anything and press Enter — the server sends the same bytes straight back. Press Ctrl+D
to disconnect, then run `nc` again to confirm the server accepts a fresh client. Ctrl+C in
terminal 1 shuts the server down.

A non-interactive one-liner, if you prefer:

```bash
printf 'hello' | nc -w1 localhost 6379
```

(`-w1` makes `nc` give up one second after stdin ends, so it does not hang waiting for more.
On the OpenBSD `nc` found on many Linux distributions, `-N` is the cleaner equivalent — it
half-closes the socket at EOF. Apple's `nc` uses `-N` for something else entirely.)

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
| 2 | TCP in the real server: accept clients sequentially, echo bytes | **Done** |
| 3 | RESP protocol: parse requests, serialize replies | Planned |
| 4 | Core commands: `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS` | Planned |
| 5 | Key-value store: the hash table behind the commands | Planned |
| 6 | Expiration: `EXPIRE`, `TTL`, lazy and active eviction | Planned |
| 7 | Event loop: non-blocking I/O, many concurrent clients | Planned |
| 8 | Richer data types: lists, hashes, sets | Planned |
| 9 | Persistence: snapshotting, and an append-only log | Planned |
| 10 | Benchmarks and tuning | Planned |

Stage boundaries may shift; the table records the intended order.

## Layout

```
├── CMakeLists.txt   the whole build
├── src/             server sources (main.cpp, server.cpp, server.hpp)
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
