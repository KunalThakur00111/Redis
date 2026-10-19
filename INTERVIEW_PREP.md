# Redis server interview preparation

Updated 2026-10-18. Answers under **Current answer** describe implemented code
at this date. The [three-month roadmap](ROADMAP_3_MONTHS.md) describes planned
work. Recheck this guide against the source before using it in an interview;
never present a planned feature or unmeasured performance number as delivered.

## Thirty-second project description

"I am building a Redis-compatible server in C++17 on Linux. It uses nonblocking
TCP sockets and a level-triggered epoll loop to serve multiple clients, parses
RESP2 requests, and runs commands against one in-memory typed keyspace. It
currently supports selected string, list, and expiry operations. I compare
behavior with Redis 8.2.9 and test the socket lifecycle. I am extending it
toward persistence, deliberate multithreading, replication, and static cluster
routing. The README names the supported subset and limitations."

Use the present tense only for features verified in the repository. Update the
description as the roadmap milestones land.

## Questions about the current implementation

### 1. Walk me through one command, from TCP bytes to reply.

**Current answer:** `main.cpp` parses the port and calls `net::run_server`.
`server.cpp` accepts a client, registers its nonblocking socket with epoll,
receives bytes into that client's input buffer, and calls `parse_request` until
no complete request remains. `execute_command` changes or reads the shared
`Database`. The encoded RESP reply is appended to that client's output buffer.
When epoll reports writable readiness, `send` transmits queued bytes; any
unsent suffix remains buffered. Pipelined requests are processed in input
order, so replies are queued in the same order.

**Be ready to trace:** `src/net/server.cpp` → `src/protocol/resp_parser.cpp` →
`src/commands/command_handler.cpp` → `src/storage/database.h` →
`src/protocol/resp_writer.cpp`.

### 2. Why epoll, and why level triggering?

**Current answer:** One thread can wait for readiness from many sockets without
blocking on one idle client. Level triggering reports a still-ready socket
again, which fits this server's bounded accept and read work per event. The
tradeoff is that an unconsumed readiness condition can wake the loop
repeatedly. epoll is a readiness API; it does not itself execute commands in
parallel. We chose it before considering an io_uring rewrite because we have
not measured a bottleneck that requires a different I/O API.

### 3. What do nonblocking mode and `EAGAIN` mean here?

**Current answer:** `fcntl(F_GETFL)` reads the descriptor flags, then
`fcntl(F_SETFL, flags | O_NONBLOCK)` preserves existing flags while adding
nonblocking mode. `accept`, `recv`, or `send` may then return `-1` with
`EAGAIN`/`EWOULDBLOCK` when progress would require waiting. The loop keeps
state and retries when epoll reports readiness. `EINTR` means a signal
interrupted a call; the server handles it without treating the connection as
a permanent failure. A positive `send` result can still be shorter than the
queued reply, so the unsent bytes must remain in the output buffer.

### 4. How do partial requests, pipelining, and client half-close work?

**Current answer:** The RESP parser reports incomplete, complete, or invalid.
Incomplete bytes remain in the client's input buffer. After a complete
request, its consumed prefix is removed and parsing continues, allowing
multiple requests in one `recv`. A read-side EOF marks the client as
`read_closed`; queued output is sent before the socket is closed. Invalid RESP
queues one protocol error and closes after that reply is flushed.

### 5. Where is backpressure, and what is its limitation?

**Current answer:** Each client has 128 KiB pending-input and pending-output
limits. Accepts are capped at 16 per listener event and successful reads at
four per client event. This bounds some per-client work and queued bytes. If a
reply would exceed the output limit, the server closes that client; it does
not stream a large response in smaller application-level pieces. There is no
global memory limit or eviction policy yet.

### 6. How are different value types represented?

**Current answer:** A database entry stores a `std::variant` containing either
`std::string` or `std::deque<std::string>`, plus an optional absolute expiry
deadline. String commands use `std::get_if<std::string>` and return `WRONGTYPE`
for a list. `LPUSH` uses a deque because adding to its front is efficient;
`LRANGE` copies the selected elements into a reply vector. A key can be
overwritten by `SET`, changing its type. Hashes, sets, and sorted sets are not
implemented yet.

### 7. How does expiration work, and can `DBSIZE` be surprising?

**Current answer:** `find_live` passively deletes a key whose absolute Unix
millisecond deadline has passed. The server also asks `expire_some(16)` to
inspect a bounded number of expiring keys after each epoll wait cycle. The
expiry vector, reverse-position map, and cursor avoid scanning the entire
database every cycle. `DBSIZE` reads the map size, so it may temporarily count
an expired key before the active sweep reaches it. That is a documented
current behavior, not something we silently claim to match Redis exactly.

### 8. What is the concurrency model today?

**Current answer:** One event-loop thread executes commands against one shared
database. This makes each command's database work run without interleaving
from another thread. It also means a slow command can delay all clients, and
the database is not safe for parallel access. Adding I/O threads requires
explicit connection ownership and an execution boundary; parallel commands
would additionally need same-key ordering and coordination for multi-key
operations.

### 9. How do you verify compatibility?

**Current answer:** CTest runs parser and socket tests. A separate differential
PING test compares raw RESP2 replies with Redis 8.2.9 when the reference
binary is installed. Selected newer commands have had focused Redis 8.2.9
comparisons, but the repository does not yet have a broad automated
differential suite. `COMPATIBILITY.md` distinguishes having a handler from
comprehensive comparison. Never say "fully Redis-compatible" on this basis.

### 10. What are the most important current limitations?

**Current answer:** No persistence: restarting loses all data. One execution
thread limits CPU scaling. RESP2 request support covers arrays of bulk
strings, with explicit parser and buffer limits; RESP3 and many Redis commands
are absent. There is no authentication, so binding to `0.0.0.0` requires a
trusted network. There is no replication, cluster routing, transaction
engine, global memory policy, or graceful shutdown path. Large replies can
hit the per-client output limit. State these clearly; do not hide them behind
the phrase "in progress."

## Questions to prepare as future milestones land

The following are **design prompts, not current implementation claims**.
Add measured and code-backed answers after each feature is complete.

| Question | What a strong answer must demonstrate |
| --- | --- |
| Why choose an append-only log, and when do you acknowledge a write? | State logging order, replay format, fsync policy, possible data loss, and handling of a torn final record. Show a restart test. |
| How do I/O workers interact with one shared database? | Name the owner of every client socket and the executor of each command. Explain queue bounds, reply order, shutdown, and a race test. |
| Why or why not parallelize command execution? | Show a benchmark bottleneck, ownership or locking model, same-key ordering, and multi-key atomicity rule. |
| How does a new replica catch up and stay in order? | Demonstrate initial state transfer, the ordered write stream, read-only behavior, disconnect, and reconnect. Say whether partial resync exists. |
| What does the cluster actually implement? | Explain slot calculation, hash tags, ownership, `MOVED`, and same-slot checks. State clearly whether node discovery, live migration, and automatic failover exist. |
| What happens when the leader crashes between a client reply and replica receipt? | State the actual durability and replication guarantees; asynchronous replication may lose an acknowledged write. Show a fault test if claiming otherwise. |
| What do your performance numbers mean? | Name hardware, clients, pipeline depth, payload size, command mix, persistence setting, throughput, and latency percentiles. Never give numbers without a reproducible workload. |

Redis's own [replication documentation](https://redis.io/docs/latest/operate/oss_and_stack/management/replication/)
and [Cluster specification](https://redis.io/docs/latest/operate/oss_and_stack/reference/cluster-spec/)
are useful references when answering what is implemented and what is not.

## Practice and evidence checklist

For each new feature, prepare a two-minute explanation: the problem, the
design choice, one concrete failure case, the test that catches it, and one
limitation. Be able to trace one request and one state change in the actual
source. Keep a reproducible demo command, test result, and benchmark setup
with the repository. If asked about AI assistance, describe your actual role
in design, review, testing, and debugging honestly; demonstrate that you can
explain and change the code yourself.
