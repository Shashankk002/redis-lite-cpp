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

**Stages 0–3 complete. No Redis commands are executed yet.**

What exists today:

- A CMake build (C++17, warnings enabled) producing four executables.
- `redis-lite-server`, which now listens on **127.0.0.1:6379**, accepts one client at a time,
  echoes back whatever it receives, and keeps accepting new clients until Ctrl+C. It does not
  understand a single Redis command yet.
- A RESP2 parser and serializer (`src/resp.hpp`, `src/resp.cpp`). It is **not connected to
  the server yet** — it is a self-contained protocol layer covered by unit tests.
- `redis-lite-tests`, a plain test executable wired into CTest (89 checks).
- `tcp-echo-server` and `tcp-echo-client` in `experiments/` — the Stage 1 exercise, kept as-is.

What does **not** exist yet: commands, key-value storage, TTL/expiration, persistence, and any
form of concurrency. The server is **sequential**: a second client waits in the kernel's
backlog until the first one disconnects. Every stage below marked *planned* describes intent,
not shipped code.

## Stage 3: the RESP protocol

### Why a protocol at all

TCP moves bytes. That is the whole of its job — it guarantees the bytes arrive, in order,
without corruption, and nothing more. In particular it has **no idea where one message ends
and the next begins**. A `recv()` call may hand you half a request, exactly one request, or
three and a half requests glued together, and none of those cases look any different from the
inside.

So every application built on TCP has to answer one question for itself: *where does this
message end?* RESP (REdis Serialization Protocol) is Redis's answer. It is a small,
line-oriented, text-ish format where the first byte of every value says what type it is, and
the value carries enough information — a terminator or a length — to know where it stops.

That is the difference between the two layers. TCP gives us `*2\r\n$3\r\nGET\r\n$4\r\nname\r\n`
as a run of 25 bytes. RESP tells us those bytes mean *an array of two bulk strings, "GET" and
"name"* — which is a command, waiting to be executed.

### The types we implemented

| Prefix | Type | Example |
| ------ | ---- | ------- |
| `+` | Simple string | `+OK\r\n` |
| `-` | Error | `-ERR unknown command\r\n` |
| `:` | Integer | `:100\r\n` |
| `$` | Bulk string | `$5\r\nhello\r\n`, empty `$0\r\n\r\n`, null `$-1\r\n` |
| `*` | Array | `*2\r\n$3\r\nGET\r\n$4\r\nname\r\n`, empty `*0\r\n`, null `*-1\r\n` |

Arrays nest, because the parser is recursive. Bulk strings are read by length rather than by
scanning for a terminator, so their contents may contain `\r\n` or NUL bytes.

### Handling partial reads

`parse()` reports one of three outcomes, and the middle one is the point of this stage:

- **Ok** — a complete value, plus how many bytes it used.
- **Incomplete** — valid so far, but truncated. Not an error: read more and try again.
- **Malformed** — genuinely invalid. More bytes will not rescue it.

The caller keeps a buffer, appends whatever `recv()` returned, and re-parses. So a request
split across two reads —

```
read 1:  *2\r\n$3\r\nGET\r\n      -> Incomplete
read 2:  $4\r\nname\r\n           -> Ok, once appended to the buffer
```

— is handled without the parser ever seeing it as broken.

### Not implemented yet

Nothing executes commands. Parsing `GET name` produces an array of two bulk strings and stops
there; there is no key-value store, no dispatch, and the TCP server still echoes raw bytes
without consulting this code at all. Wiring the two together is a later stage.

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
| 3 | RESP protocol: parse requests, serialize replies | **Done** |
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
├── src/             server sources and the RESP protocol layer
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
