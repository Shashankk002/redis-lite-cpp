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

**Stages 0–9 complete. A single-threaded, event-driven server with four data types, TTL and append-only persistence.**

What exists today:

- A CMake build (C++17, warnings enabled) producing four executables.
- `redis-lite-server`, which listens on **127.0.0.1:6379**, serves **many clients at once from
  a single thread** using a kqueue event loop, and executes `PING`, `SET`, `GET`, `DEL`,
  `EXPIRE` and `TTL` over RESP, plus list, hash and set commands, until Ctrl+C. Data survives a
  restart via an append-only log.
- A RESP2 parser and serializer (`src/resp.hpp`, `src/resp.cpp`), now driving the server.
- A command layer (`src/commands.hpp`, `src/commands.cpp`) holding the key-value store and
  its expiration deadlines.
- `redis-lite-tests`, a plain test executable wired into CTest (462 checks).
- `tcp-echo-server` and `tcp-echo-client` in `experiments/` — the Stage 1 exercise, kept as-is.

What does **not** exist yet: sorted sets and the rest of the Redis command set, log compaction,
and portability beyond macOS/BSD (see the note on `epoll` below).
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

This stage's architecture was replaced in Stage 7; see below. The demonstration script is now
`./demo_event_loop.sh`.

## Stage 7: event-driven I/O with kqueue

### Why one thread per client does not scale

Stage 6 gave every connection its own thread. It works, and it is easy to read, but each thread
reserves a large stack (commonly 512 KB to 8 MB) and the kernel must schedule all of them.
Ten thousand mostly-idle clients means ten thousand threads whose only job is to sit blocked in
`recv()` — an entire OS thread used to represent *waiting*.

The insight behind event-driven I/O is that waiting does not need a thread. It needs a list.

### Non-blocking sockets

By default a socket is **blocking**: `recv()` does not return until data arrives. A single
thread calling `recv()` on client A is therefore stuck until A speaks, and cannot serve anyone
else — the exact problem Stage 6 solved with threads.

`fcntl(fd, F_SETFL, flags | O_NONBLOCK)` changes that. A non-blocking `recv()` with no data
available returns `-1` immediately with `errno == EAGAIN` (equivalently `EWOULDBLOCK`), which
means "nothing right now, ask again later" rather than "an error occurred". The same applies to
`accept()` and `send()`.

That alone would only let us *poll* every socket in a busy loop, burning CPU. What is missing is
a way to sleep until something is actually ready.

### What kqueue is

`kqueue` is the kernel's readiness-notification mechanism on macOS and the BSDs. You create one
with `kqueue()` — it is itself a file descriptor — and then use `kevent()` for two purposes:

- **registering interest**: "tell me when descriptor 7 is readable"
- **waiting**: "sleep until any of the descriptors I registered is ready, then hand me the list"

The kernel already knows which sockets have data, so it can answer this without the process
polling anything. One thread can therefore supervise thousands of connections and be woken only
for the handful that actually need work.

```cpp
const int kq = kqueue();

struct kevent change;
EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
kevent(kq, &change, 1, nullptr, 0, nullptr);          // register

const int ready = kevent(kq, nullptr, 0, events.data(), events.size(), nullptr);   // wait
```

**`EVFILT_READ`** means "this descriptor has something to read". On the *listening* socket that
means a client is waiting to be accepted; on a *client* socket it means bytes have arrived (or,
with `EV_EOF`, that the client hung up).

**`EVFILT_WRITE`** means "there is room in this socket's send buffer". We register it **only
when we have data we could not write**, and remove it as soon as the backlog drains — a socket
with an empty send buffer is writable essentially always, so leaving the filter registered would
wake the loop continuously for no reason.

### The event loop

```
while (running) {
    ready = kevent(...)                  // sleeps until something happens

    for each ready event:
        if it is the listening socket:
            accept() until EAGAIN, register each new client for EVFILT_READ
        else if EVFILT_READ:
            recv() until EAGAIN, append to that client's input buffer
            run every complete request in the buffer, append replies to its output buffer
            try to send
        else if EVFILT_WRITE:
            send what is pending; if the buffer empties, drop the write filter
}
```

Accepting **until `EAGAIN`** matters: several clients may have connected between two iterations,
and the readiness notification only says "at least one", not how many.

### Why each client needs its own buffers

In Stage 6, `handle_client` was a function running on a dedicated thread, so a plain local
variable could hold that connection's partially-received bytes — the thread's stack *was* the
per-connection state.

There is no such thread now. The event loop touches a client, returns to the loop, and may not
come back for a while. So the state has to be stored explicitly:

```cpp
std::unordered_map<int, std::string> client_input;   // fd -> received, not yet parsed
std::unordered_map<int, std::string> client_output;  // fd -> owed to the client
```

Both exist because TCP is a byte stream in both directions. An **input** buffer is needed because
one request may arrive across several reads (`*2\r\n$3\r\nGET\r\n` now, `$3\r\nfoo\r\n`
later), and because one read may contain several requests. An **output** buffer is needed
because a non-blocking `send()` may accept only part of a large reply and then return `EAGAIN`;
the remainder waits until `EVFILT_WRITE` says there is room.

### Why the mutex is gone

Stage 6 needed a mutex because several threads could reach into the store at the same moment.
There is now exactly one thread again, so commands run strictly one after another and no
interleaving is possible:

```
Stage 6:   thread A --+
                      +--> shared Store      -> mutex required
           thread B --+

Stage 7:   event loop thread --> Store       -> no mutex needed
```

**This is concurrency in the networking, not in the execution.** Many connections are open and
progressing at once, and the kernel tells the loop which ones need attention — but each command
is executed sequentially on the one thread. Redis-Lite does not run two commands in parallel,
and it never did: even in Stage 6 the mutex serialised them.

### Why Linux would use epoll

`kqueue` is a BSD interface: macOS, FreeBSD, OpenBSD, NetBSD. Linux does not have it, and uses
`epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`) for the same job; Windows uses I/O
completion ports, which is a different model again. The *architecture* here — non-blocking
sockets, readiness notification, one event loop, per-connection buffers — is the same on all
three. Only the three or four syscalls differ. **This implementation is macOS/BSD-only.**

### Known limitations

- **macOS/BSD only.** No `epoll` fallback, so this does not build meaningfully on Linux.
- **One event loop, one core.** A CPU-heavy command would stall every client, since there is no
  other thread to run them. Real Redis has the same property and accepts it.
- **Level-triggered, and buffers grow without limit.** A client can send an enormous bulk-string
  header and the input buffer will keep growing; there is no maximum request size and no output
  backpressure. Real servers cap both.
- **No timer events.** Expiration is still lazy: an expired key is only removed when something
  asks for it. kqueue has `EVFILT_TIMER`, which is the natural way to add active expiration, but
  that is deliberately left for later.
- **`accept()` is not rate-limited**, so a flood of connections can monopolise one iteration.

### Demonstration

```bash
./demo_event_loop.sh
```

Shows an idle client not blocking others, shared state between clients, simultaneous clients,
pipelining, a request split across two writes, a 200 KB reply that needs several writes,
malformed input surviving, reconnection, and TTL.

## Stage 8: persistence with an append-only log

### Why the data vanished before

Everything lived in `std::unordered_map`s inside the process. When the process exits — Ctrl+C,
a crash, a reboot — that memory is returned to the OS and every key is gone. Nothing had ever
been written to disk.

### What an append-only log is

The simplest durable design is not to save the *data*, but to save the *changes*. Every command
that modifies state is appended to a file, in order. To rebuild the store you replay them:

```
SET 3 foo 3 bar          ->  foo = "bar"
SET 5 count 2 42         ->  count = "42"
DEL 3 foo                ->  foo removed
EXPIRE 5 count 1772456789    ->  count expires at that Unix second
```

Appending is cheap (no seeking, no rewriting), and the file is a complete history. This is the
same idea as Redis's AOF, though the format here is our own and deliberately simpler.

### The record format

```
SET    <keylen> <key> <valuelen> <value>\n
DEL    <keylen> <key>\n
EXPIRE <keylen> <key> <unix-deadline-seconds>\n
```

Keys and values are **length-prefixed**, and that is the important part. Redis values are
binary-safe: they may contain spaces, newlines, even NUL bytes. A delimiter-based format would
break on all of those. Because the reader is told to take exactly *N* bytes, any byte sequence
survives the round trip — `SET 5 a key 7 a value` is unambiguous, and so is a value that happens
to contain `\n` or looks like a record itself.

The file stays readable in a text editor for ordinary keys, which makes it easy to inspect.

### Which commands are recorded, and which are not

Recorded: **`SET`**, **`DEL`**, **`EXPIRE`** — the commands that change state.

Not recorded: **`GET`**, **`TTL`**, **`PING`** — they only read. Replaying a `GET` would
accomplish nothing, and logging reads would make the file grow without adding information.

Two policy details:

- **`DEL` is recorded only when it actually removed something.** A `DEL` on a missing key
  changes nothing, so there is nothing to reconstruct.
- **`EXPIRE key 0`** (or a negative time) deletes the key, so it is recorded as a `DEL` — the
  record describes the effect, not the command that was typed.

### Why `steady_clock` cannot be persisted

Stage 5 chose `steady_clock` for expirations because it is monotonic: it cannot jump when the
wall clock is corrected. But its zero point is arbitrary — usually when the machine booted — and
it means nothing in another process. Writing `steady_clock::time_point{4213377}` to disk and
reading it back tomorrow would produce a deadline in the far past or far future at random.

So the log stores an **absolute Unix timestamp** from `system_clock`, which has a fixed,
universally agreed zero point. On replay the server subtracts the current wall-clock time to get
the remaining seconds, and hands *that duration* to the ordinary `EXPIRE` path, which converts it
back into a `steady_clock` deadline:

```
on write:   deadline_unix = now_unix + seconds
on replay:  remaining = deadline_unix - now_unix
            remaining <= 0  ->  the key died while we were down: delete it
            remaining >  0  ->  EXPIRE key remaining
```

The in-memory logic is untouched: it still uses `steady_clock`, which is still the right choice
for measuring an interval inside one process. Only the on-disk representation is wall-clock.

### Replay must not write back into the log

Replay runs each record through the same `execute_command()` a network client would reach — which
is what guarantees the semantics match exactly, including "`SET` clears an existing TTL" and
"`DEL` removes the expiration too". But those commands would normally *append to the log*, so
replaying a 10-record file would write 10 more records, and the next start would write 20.

The mechanism that prevents it is deliberately tiny: `execute_command` takes a `std::ofstream*`,
and replay passes `nullptr`.

```cpp
RespValue execute_command(const RespValue& request, Store& store, std::ofstream* log = nullptr);
```

A null log records nothing. No flags, no modes, no persistence class.

### flush vs fsync

After each record the server calls `log.flush()`. That empties **the C++ stream's own buffer**
into the operating system. It is *not* `fsync()`, which asks the OS to push its page cache down
to the physical device.

So: if the **process** dies (crash, `kill -9`), the flushed records are safe, because the OS has
them. If the **machine** loses power, recently written records may still be lost, because they
may not have reached the disk yet. Real Redis exposes this as a choice (`appendfsync
always/everysec/no`) precisely because `fsync` on every write is slow. This is a learning
implementation, not a crash-consistent database, and it stops at `flush`.

### The file grows forever

This log is append-only with no compaction, so

```
SET foo bar
SET foo baz
SET foo qux
```

leaves all three records even though only the last matters. A long-running server's log grows
without bound, and startup gets slower as it grows. Real Redis solves this with **AOF rewrite**:
periodically writing a fresh, minimal log describing the current state. That is a future
improvement and is not implemented here.

### Other limitations

- **No compaction, rewriting, snapshots or rotation** — see above.
- **`flush`, not `fsync`** — see above.
- **A corrupt log is fatal.** If any record is malformed, the server prints the record number and
  the reason and refuses to start, rather than loading part of the data and pretending it is
  complete. Recovery is manual: fix or remove the file. There is no truncate-and-continue mode.
- **Writes are synchronous.** The append happens inline in the event loop, so a slow disk stalls
  every client. Asynchronous disk I/O is out of scope.
- **Lazy expiration means expired keys stay in the log.** A key that expired is dropped at replay
  time, but its records remain in the file until a compaction that does not exist yet.

### Trying it

```bash
./demo_persistence.sh
```

Or by hand — the log path is an optional argument, defaulting to `redis-lite.aof` in the working
directory:

```bash
./build/redis-lite-server /tmp/my.aof
# ... SET some keys, then Ctrl+C ...
cat /tmp/my.aof
./build/redis-lite-server /tmp/my.aof     # the keys are back
```

## Stage 9: lists, hashes and sets

### The data model

Until Stage 8 every key held a string. Now a key holds exactly one of four things:

```cpp
using StringValue = std::string;
using ListValue   = std::deque<std::string>;
using HashValue   = std::unordered_map<std::string, std::string>;
using SetValue    = std::unordered_set<std::string>;

using Value = std::variant<StringValue, ListValue, HashValue, SetValue>;

struct Store {
    std::unordered_map<std::string, Value> values;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations;
};
```

`std::variant` is doing real work here. The central rule of this stage — *a key cannot hold two
types at once* — is not a rule the code has to remember to enforce; a variant holds exactly one
alternative and there is no way to write down a key that is both a list and a hash.
`std::get_if<T>` returns a null pointer for the wrong alternative, which maps directly onto the
one error this stage needs.

A `deque` for lists, rather than a `vector`, because `LPUSH`/`LPOP` work at the front: every
left-hand push into a vector would shuffle the entire list.

### Supported commands

| Type | Commands |
| ---- | -------- |
| Connection | `PING` |
| Generic | `DEL`, `EXPIRE`, `TTL` — these work on any type |
| String | `SET`, `GET` |
| List | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE` |
| Hash | `HSET`, `HGET`, `HDEL`, `HEXISTS`, `HLEN` |
| Set | `SADD`, `SREM`, `SISMEMBER`, `SCARD`, `SMEMBERS` |

`LRANGE` supports negative indices the way Redis does: `-1` is the last element, so
`LRANGE key 0 -1` is the whole list, and out-of-range bounds are clamped rather than rejected.

Each of these takes a **single** value, field or member — `LPUSH key a b c` is not supported.
Redis allows the variadic form; adding it is mechanical and was left out to keep the stage
focused on the type system rather than on argument parsing.

### Wrong-type semantics

Using a command against a key holding another type returns the Redis error verbatim:

```
WRONGTYPE Operation against a key holding the wrong kind of value
```

Three rules make this predictable:

- **A rejected command changes nothing.** It does not create the key, and it does not disturb
  the existing value. `LPUSH` on a string leaves the string exactly as it was.
- **`SET` is the exception, and replaces any type.** `SET key value` over a list turns the key
  into a string and clears its TTL, matching Redis.
- **Every other type transition requires an explicit `DEL` first**, or emptying the collection.

`DEL`, `EXPIRE` and `TTL` are type-agnostic: they operate on the key, not on what it holds, so
they never return WRONGTYPE.

**Empty collections do not exist.** Popping the last element of a list, or removing the last
field or member, deletes the key outright — so `LLEN` on it reads `0`, `TTL` reads `-2`, and the
name is free to be reused for a different type. This matches Redis and is what keeps
"missing key" and "empty collection" from being two different states to reason about.

### TTL works on every type

Expiration is a property of the key, so `EXPIRE mylist 60` is as valid as `EXPIRE mystring 60`.
When the deadline passes, the whole collection disappears at once, and every command sees it as
missing: `LLEN` gives `0`, `LRANGE` gives an empty array, `TTL` gives `-2`. The lazy-expiration
mechanism from Stage 5 is unchanged — nothing scans in the background.

### Persistence

The Stage 8 format was already "a verb followed by length-prefixed arguments", so it generalised
without changing a single existing byte. Old logs still load.

```
SET    <klen> <key> <vlen> <value>       LPUSH  <klen> <key> <vlen> <value>
DEL    <klen> <key>                      RPUSH  <klen> <key> <vlen> <value>
EXPIRE <klen> <key> <unix-deadline>      LPOP   <klen> <key>
                                         RPOP   <klen> <key>
HSET   <klen> <key> <flen> <field> <vlen> <value>
HDEL   <klen> <key> <flen> <field>
SADD   <klen> <key> <mlen> <member>
SREM   <klen> <key> <mlen> <member>
```

The reader is now a small table of verb → argument count, so adding a command means adding one
row rather than another parsing branch. `EXPIRE` remains the one special case, because its tail
is a number rather than arbitrary bytes.

Two things fall out of replaying through the *same* `execute_command` a client reaches:

- **Emptied collections are handled for free.** Replaying the `LPOP` that emptied a list empties
  it again, and the same `drop_if_empty` removes the key. No `DEL` record is needed.
- **Only real changes are recorded.** A duplicate `SADD`, an `HDEL` of an absent field, or an
  `SREM` of a non-member change no state, so nothing is written.

Replay still passes `nullptr` as the log, so it never writes back into the file it is reading.

### Trying it

```bash
./demo_datatypes.sh
```

Or by hand — `nc` speaks RESP if you feed it the right bytes:

```bash
# RPUSH fruits apple
printf '*3\r\n$5\r\nRPUSH\r\n$6\r\nfruits\r\n$5\r\napple\r\n' | nc -w2 127.0.0.1 6379

# LRANGE fruits 0 -1
printf '*4\r\n$6\r\nLRANGE\r\n$6\r\nfruits\r\n$1\r\n0\r\n$2\r\n-1\r\n' | nc -w2 127.0.0.1 6379
```

`demo_datatypes.sh` contains a `resp()` shell function that builds these for you.

### Design tradeoffs

**`std::variant` over a struct with a type tag.** Stage 3's `RespValue` uses the tagged-struct
pattern, and consistency argued for repeating it. But there, the alternatives were tiny and the
invariant was not the point. Here a struct would carry an empty `deque`, `unordered_map` *and*
`unordered_set` on every key — well over a hundred bytes of dead weight on a plain string — and,
worse, nothing would stop code from reading the wrong member for the tag. The variant makes the
illegal state unrepresentable, which is precisely what this stage is about. The cost is
`std::get_if` at each use, and a variant as large as its biggest alternative.

**Two small function templates** (`read_typed` / `write_typed`) rather than sixteen copies of
the same lookup-and-check. `read_typed` deliberately reports "missing" and "wrong type"
separately, because they have different replies.

**`unordered_map`/`unordered_set` for hashes and sets** means `SMEMBERS` returns members in an
unspecified order. Redis makes the same guarantee (none), so this is correct — but it means
tests must compare members as a set, which they do.

### Known limitations

- **Single-value commands only.** No `LPUSH key a b c`, no `HSET key f1 v1 f2 v2`, no multi-key
  `DEL`.
- **No sorted sets**, and none of `LINSERT`, `LSET`, `LREM`, `HGETALL`, `HKEYS`, `HVALS`,
  `SINTER`, `SUNION`, `SDIFF`, `SPOP`, `TYPE`, `EXISTS`, or `KEYS`.
- **No per-type encodings.** Real Redis switches representation by size (ziplist/listpack for
  small collections, hashtable for large). Everything here is one representation per type.
- **`SMEMBERS` and `LRANGE` build the whole reply in memory** before sending, so a very large
  collection produces a very large output buffer.
- **The log still grows forever** — a list that is pushed and popped a million times keeps all
  two million records. Compaction remains the future improvement it was in Stage 8.

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
| 7 | Event loop: non-blocking I/O with kqueue, single-threaded | **Done** |
| 8 | Persistence: an append-only log | **Done** |
| 9 | Richer data types: lists, hashes, sets | **Done** |
| 10 | Benchmarks and tuning | Planned |

Stage boundaries may shift; the table records the intended order.

## Layout

```
├── CMakeLists.txt   the whole build
├── src/             server sources and the RESP protocol layer
├── include/         shared headers (empty until a header is needed)
├── tests/           test executable, run via CTest
├── experiments/     standalone learning exercises, not part of the server
├── demo_event_loop.sh   scripted demonstration of the event loop
├── demo_persistence.sh  scripted demonstration of restart persistence
├── demo_datatypes.sh    scripted demonstration of lists, hashes and sets
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
