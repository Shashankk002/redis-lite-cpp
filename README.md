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

**Stages 0–6 complete. A threaded server speaking Redis for a six-command subset, with TTL.**

What exists today:

- A CMake build (C++17, warnings enabled) producing four executables.
- `redis-lite-server`, which listens on **127.0.0.1:6379**, serves **many clients at once**
  (one thread each), and executes `PING`, `SET`, `GET`, `DEL`, `EXPIRE` and `TTL` over RESP
  until Ctrl+C.
- A RESP2 parser and serializer (`src/resp.hpp`, `src/resp.cpp`), now driving the server.
- A command layer (`src/commands.hpp`, `src/commands.cpp`) holding the key-value store and
  its expiration deadlines.
- `redis-lite-tests`, a plain test executable wired into CTest (200 checks).
- `tcp-echo-server` and `tcp-echo-client` in `experiments/` — the Stage 1 exercise, kept as-is.

What does **not** exist yet: every other Redis command, persistence, and event-driven I/O.
Every stage below marked *planned* describes intent, not shipped code.

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

## Stage 4: executing commands

### What a "command" actually is

Stage 3 turned bytes into a `RespValue`. That is still just data — knowing the client sent an
array of two bulk strings says nothing about what should happen. Command execution is the step
that gives those values *meaning*: the first element of the array is the command name, and the
rest are its arguments.

```
["GET", "name"]   ->  command = GET, key = name
```

Redis command names are case-insensitive, so the name is upper-cased before dispatch: `ping`,
`PING` and `PiNg` are the same command. Keys are **not** normalised — `Key` and `key` are two
different keys, exactly as in Redis.

### The commands

| Command | Request | Reply |
| ------- | ------- | ----- |
| `PING` | `*1\r\n$4\r\nPING\r\n` | `+PONG\r\n` |
| `SET key value` | `*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nShashank\r\n` | `+OK\r\n` |
| `GET key` (exists) | `*2\r\n$3\r\nGET\r\n$4\r\nname\r\n` | `$8\r\nShashank\r\n` |
| `GET key` (missing) | same | `$-1\r\n` (null bulk string) |
| `DEL key` (existed) | `*2\r\n$3\r\nDEL\r\n$4\r\nname\r\n` | `:1\r\n` |
| `DEL key` (missing) | same | `:0\r\n` |

Anything else is answered with a RESP error rather than a crash:

```
unknown command       -ERR unknown command 'FLUSHALL'
wrong argument count  -ERR wrong number of arguments for 'get' command
not an array          -ERR expected a non-empty array of bulk strings
broken RESP           -ERR Protocol error: unknown RESP type byte   (connection then closed)
```

### The request/response flow

```
client
  |  bytes
  v
recv()                      may deliver half a command, or three commands
  |
append to per-client buffer
  |
parse(buffer)               Stage 3
  |-- Incomplete  -> go back to recv() and wait for more
  |-- Malformed   -> reply with an error, close the connection
  |-- Ok          -> a RespValue, plus how many bytes it used
  v
execute_command(value, store)
  |
serialize(reply)            Stage 3
  |
send()
  |
buffer.erase(0, consumed)   leftovers are the next command
  |
loop
```

The buffer is the part that matters. TCP has no message boundaries, so the server never
assumes one `recv()` equals one command: it appends what arrived, executes every *complete*
command in the buffer, and keeps the remainder for next time.

### Storage

A single `std::unordered_map<std::string, std::string>`, created in `run_server` and passed by
reference into each connection. It lives as long as the process, so keys survive a client
disconnecting — and vanish entirely when the server stops. There is no persistence.

Because the server handles one client at a time, no locking is needed. That stops being true
the moment concurrency arrives.

### Not implemented yet

Every other Redis command, TTL and expiration, persistence (RDB/AOF), authentication,
transactions, pub/sub, replication, and concurrency of any kind. A second client connecting
while another is mid-conversation waits in the kernel's backlog.

### Trying it by hand

There is no `redis-cli` dependency here — RESP is simple enough to type:

```bash
# terminal 1
./build/redis-lite-server

# terminal 2
printf '*1\r\n$4\r\nPING\r\n' | nc -w2 127.0.0.1 6379
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nShashank\r\n' | nc -w2 127.0.0.1 6379
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -w2 127.0.0.1 6379
printf '*2\r\n$3\r\nDEL\r\n$4\r\nname\r\n' | nc -w2 127.0.0.1 6379
```

Several commands in a single connection, to watch the buffer loop work:

```bash
printf '*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n' | nc -w2 127.0.0.1 6379
```

If you do have `redis-cli` installed, `redis-cli -p 6379 ping` also works — Redis-Lite speaks
the same wire protocol, just far fewer commands.

## Stage 5: key expiration (TTL)

### What a TTL is

A TTL — time to live — is a deadline attached to a key. Once it passes, the key behaves as
though it was never there: `GET` returns null, `DEL` reports 0, `EXPIRE` reports 0. Redis uses
this constantly for caches and sessions, where data should disappear on its own rather than
being cleaned up by hand.

| Command | Result |
| ------- | ------ |
| `EXPIRE key seconds` | `:1` if the key exists, `:0` if it does not |
| `EXPIRE key 0` or a negative value | key is deleted immediately, replies `:1` |
| `TTL key` | remaining whole seconds |
| `TTL key` — key exists, no expiration | `:-1` |
| `TTL key` — key missing or already expired | `:-2` |

Two rules are easy to miss, and both are tested:

- **`SET` clears an existing TTL.** `SET foo bar` / `EXPIRE foo 10` / `SET foo new` leaves
  `TTL foo` at `-1`. Rewriting a key gives it a fresh, unlimited life.
- **`DEL` removes the deadline too**, not just the value — otherwise a stale deadline would
  silently kill the next key created with the same name.

### How the deadline is stored

The store is now a plain struct holding two maps:

```cpp
struct Store {
    std::unordered_map<std::string, std::string> values;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations;
};
```

A key with no entry in `expirations` never expires, which is the common case — most keys
never get a TTL, so the second map stays small. What is stored is an **absolute deadline**
(`now + seconds`), not a duration, so no countdown has to be maintained: `TTL` simply
subtracts the current time from the stored deadline.

### Why `steady_clock`

`system_clock` is the wall clock, and the wall clock can move. NTP corrections, a manual clock
change, or a timezone/DST adjustment can jump it forwards or backwards — and a key with a
10-second TTL would then expire in an hour, or instantly. `steady_clock` is monotonic: it only
ever moves forward at a steady rate, which is exactly what measuring an interval requires.

The tradeoff is that a `steady_clock` value means nothing outside this process — it usually
counts from boot, not from an epoch, so it cannot be written to disk or sent to another
machine. That matters for persistence and replication, neither of which exists here. For
measuring "has five seconds passed", it is the correct tool.

### Lazy expiration

Nothing is scanning for expired keys. Instead, every command that touches a key checks that
key's deadline first:

```
GET foo
  |
  does foo have a deadline?
  |-- no  -> return the value
  |-- yes, still in the future -> return the value
  |-- yes, already passed      -> delete the key, then answer as if it never existed
```

So an expired key is removed at the moment somebody next asks about it. The observable
behaviour is identical to expiring it on time, because no client can tell the difference
without asking.

**Why no background thread.** A timer thread or a priority queue of deadlines would reclaim
memory sooner, and that is genuinely why real Redis *also* does active expiration in its main
loop. But a background sweeper introduces concurrent access to the store, which immediately
means locking — a whole topic, and one this server does not need yet, since it handles a
single client at a time. Lazy expiration gives correct answers today with no new concepts.

The known cost: a key that expires and is never asked about again keeps its memory. Nothing
frees it until someone touches that key.

### Trying it by hand

```bash
# terminal 1
./build/redis-lite-server

# terminal 2 - set a key that lives 3 seconds, then watch it vanish
printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n' | nc -w2 127.0.0.1 6379
printf '*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$1\r\n3\r\n' | nc -w2 127.0.0.1 6379
printf '*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n'    | nc -w2 127.0.0.1 6379
sleep 4
printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'    | nc -w2 127.0.0.1 6379   # $-1
printf '*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n'    | nc -w2 127.0.0.1 6379   # :-2
```

## Stage 6: concurrent clients with threads

### The problem with the sequential server

Through Stage 5 the accept loop did this:

```
accept client A  ->  handle A until it disconnects  ->  accept client B
```

`handle_client` blocks in `recv()` waiting for A to say something. If A connects and then goes
quiet — or is simply slow, or idle — B is not merely slow to be served, it is not served *at
all*. It sits in the kernel's backlog queue until A hangs up. One idle client denies service to
everyone.

### One thread per client

Now `accept()` hands the connection to a new thread and immediately loops back to accept the
next one:

```cpp
client_threads.emplace_back(handle_client,
                            client_fd,
                            std::ref(store),
                            std::ref(store_mutex));
```

Each client gets its own `std::thread` running the same `handle_client` as before. The
per-connection buffering, parsing and command execution did not change at all — the only
difference is how many of them run at once.

### Why the store now needs a mutex

The sequential server never needed synchronisation, and it is worth being precise about why:
there was exactly **one** thread. Commands ran strictly one after another, so the store could
never be observed halfway through a modification.

```
before:   client A -> execute -> client B -> execute        (one thread, no overlap)

now:      thread A --+
                     +--> shared Store
          thread B --+                                       (two threads, real overlap)
```

`std::unordered_map` is not thread-safe for concurrent modification. Two threads inserting at
once can corrupt the bucket list, and a rehash triggered by one thread moves nodes another
thread is reading. The result is not a wrong answer, it is undefined behaviour.

A **mutex** (mutual exclusion) fixes this by allowing only one thread inside a region of code
at a time. A thread that tries to lock a mutex another thread already holds is put to sleep
until it is released. `std::lock_guard` locks on construction and unlocks on destruction, so
the lock is released even if the guarded code returns early or throws.

Both `values` and `expirations` are covered by the same single mutex, because they are one
logical state: `DEL` writes to both, and a thread must never see a key removed from one but
still present in the other.

### Why the lock is not held during network I/O

Only the store access is inside the critical section:

```cpp
RespValue reply;
{
    std::lock_guard<std::mutex> lock(store_mutex);
    reply = execute_command(result.value, store);
}
send_reply(client_fd, serialize(reply));   // outside the lock
```

`recv()` and `send()` can block for an unbounded time — a client might not send its next
command for an hour. Holding the mutex across `recv()` would mean one idle client freezing
every other client out of the store, which would re-create the exact problem threads were
introduced to solve. The mutex protects the *data*, not the socket, so it is held for
microseconds rather than minutes.

### Shutdown

Ctrl+C (or `SIGTERM`) sets the shutdown flag, `accept()` returns with `EINTR`, and the
listening socket is closed, so no new clients are accepted. The server then **joins** its
client threads, letting each connected client finish naturally.

The tradeoff is that shutdown waits for connected clients to disconnect: an idle open
connection will keep the server alive. Joining is the simple, safe choice — detaching the
threads instead would let `run_server` return while threads are still using the `Store` that
lives in its stack frame, which is undefined behaviour. Forcibly waking the threads would mean
tracking every client socket and shutting each one down, which is more machinery than this
stage needs.

### Known limitation: this is not the final architecture

One thread per client is intentionally the simplest design that works, and it is the right
thing to learn first. But:

- **Every client costs a thread**, and a thread costs memory — typically around 512 KB to 8 MB
  of stack reserved per thread. A thousand idle clients means a thousand threads.
- **Most of those threads do nothing.** They sit blocked in `recv()` waiting for input, using
  a whole OS thread to represent "waiting".
- **Context switching** between many threads costs real time, and every command serialises on
  the one mutex anyway.

Real Redis handles tens of thousands of connections with a **single** thread, by asking the
kernel which sockets have data ready (`epoll` on Linux, `kqueue` on the BSDs and macOS) and
handling only those. That is event-driven I/O, and it is what would eventually replace this.

### Demonstration script

```bash
./demo_concurrency.sh
```

It starts a server, connects a client that stays silent for 12 seconds, shows other clients
being served during that window, then demonstrates shared state, two simultaneous clients on
different keys, and TTL — then shuts everything down.

## Trying the server

```bash
# terminal 1
./build/redis-lite-server
# Redis-Lite server listening on 127.0.0.1:6379

# terminal 2
nc 127.0.0.1 6379
```

The server no longer echoes: it expects RESP. See the Stage 4 section above for commands you
can send. Ctrl+C in terminal 1 shuts the server down.

(`-w2` makes `nc` give up two seconds after stdin ends, so it does not hang waiting for more.
Use `127.0.0.1` rather than `localhost`: name resolution counts against that same timeout, and
a cold resolver cache can eat the whole budget before the connection is even made.
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
| 4 | Core commands: `PING`, `SET`, `GET`, `DEL` | **Done** |
| 5 | Expiration: `EXPIRE`, `TTL`, lazy eviction | **Done** |
| 6 | Concurrency: one thread per client, mutex-guarded store | **Done** |
| 7 | Event loop: non-blocking I/O in a single thread | Planned |
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
├── demo_concurrency.sh  scripted demonstration of concurrent clients
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
