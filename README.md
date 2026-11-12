# Redis-compatible server (in progress)

A Linux-only C++17 in-memory server. It accepts RESP arrays of bulk
strings over TCP and currently supports `PING`, `ECHO`, `SET`, `MSET`, `GET`, `DEL`,
`EXISTS`, `DBSIZE`, `MGET`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`,
`TTL`, `PTTL`, `PERSIST`, `EXPIRETIME`, `PEXPIRETIME`, `GETDEL`, `INCR`, `DECR`,
`INCRBY`, `DECRBY`, `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN`,
`LINDEX`, `HSET`, `HGET`, `HDEL`, `HLEN`, `SADD`, `SREM`, `SISMEMBER`,
`SCARD`, and `SMEMBERS`, plus:

- strings: `APPEND`, `STRLEN`, `SETNX`, `GETSET`, `INCRBYFLOAT`
- lists: `LSET`, `LREM`, `LTRIM`, `LINSERT`
- hashes: `HSET` with several fields, `HMSET`, `HSETNX`, `HGETALL`, `HKEYS`,
  `HVALS`, `HMGET`, `HEXISTS`, `HSTRLEN`, `HINCRBY`, `HINCRBYFLOAT`
- sets: `SMISMEMBER`, `SINTER`, `SUNION`, `SDIFF`, `SINTERSTORE`, `SUNIONSTORE`,
  `SDIFFSTORE`, `SPOP`, `SRANDMEMBER`, `SMOVE`
- sorted sets: `ZADD` (`NX`, `XX`, `GT`, `LT`, `CH`, `INCR`), `ZINCRBY`, `ZSCORE`,
  `ZMSCORE`, `ZCARD`, `ZREM`, `ZRANK` and `ZREVRANK` (with `WITHSCORE`), `ZRANGE`
  (`BYSCORE`, `BYLEX`, `REV`, `LIMIT`, `WITHSCORES`), `ZREVRANGE`,
  `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZCOUNT`,
  `ZLEXCOUNT`, `ZPOPMIN`, `ZPOPMAX`, `ZREMRANGEBYRANK`, `ZREMRANGEBYSCORE`,
  `ZREMRANGEBYLEX`

With `--aof`, `SAVE` and `BGREWRITEAOF` control persistence.
Admin commands: `TYPE`, `KEYS`, `SCAN` (with `MATCH`, `COUNT` and `TYPE`),
`RENAME`, `RENAMENX`, `FLUSHDB`, `FLUSHALL`, `SELECT` (database 0 only), `INFO`
(server, persistence and keyspace sections), `CONFIG GET`, and `COMMAND COUNT`,
`COMMAND LIST` and `COMMAND DOCS` (which returns no documentation, so
`redis-cli` works without command hints).
`SET` supports `NX`, `XX`, `EX`, `PX`, `EXAT`, and `PXAT`, including a condition
with an expiry.

`KEYS`, `SCAN MATCH` and `CONFIG GET` use Redis glob patterns (`*`, `?`, `[abc]`,
`[^a]`, `[a-z]`, `\x`). A `SCAN` cursor is a position in the order of each key's
64-bit hash rather than a hash-table bucket, because `std::unordered_map` can
reorder its buckets at any insert. So, as in Redis, every key present for a whole
scan is returned at least once. Unlike Redis, each `SCAN` call reads every key,
so a full scan of N keys costs about N × N / COUNT. `INFO` reports
`redis_version:8.2.9` because that is the compatibility target.

A sorted set ([src/storage/sorted_set.h](src/storage/sorted_set.h)) pairs a hash
map from member to score with a skip list ordered by score and then member
bytes, as Redis does. Each skip-list link stores how many elements it skips, so
`ZRANK`, rank ranges and `ZCOUNT` take O(log N). Scores are printed as Redis
prints them: whole numbers as integers, otherwise the shortest digits that read
back as the same double (`0.1`, `1e+20`, `0.00001`). Snapshots store each score's
8 bytes, and the AOF writes 17 significant digits, so both restore the exact
double. Each member is stored twice, in the map and in the skip list; Redis
shares one copy.

`INCRBYFLOAT` and `HINCRBYFLOAT` use `long double` and print 17 decimals with
trailing zeros removed, as Redis does. `APPEND` refuses to grow a string past
64 KiB, the request bulk-string limit, because a longer value could not be sent
back within a client's 128 KiB output limit. `SRANDMEMBER` with a negative count
larger than 1,000,000 is refused for the same reason instead of building a reply
that would be dropped.
Data is shared between clients. By default it disappears when the process exits;
the optional append-only log described below restores it on restart.

## Compatibility target

Redis Open Source 8.2.9. Implement and compare RESP2 behavior first.

The three-month roadmap and interview guide are kept locally rather than in Git.

## Architecture

```text
TCP client
   │ RESP bytes
   ▼
net/server.cpp                 epoll loop, client input/output, socket I/O
   │ complete request                   ▲ encoded reply
   ▼                                    │
protocol/resp_parser.cpp       commands/command_handler.cpp
                                │                  │
                                ▼                  ▼
                       storage/database.h   protocol/resp_writer.cpp
```

The server uses Linux POSIX sockets and level-triggered `epoll` loops.
Accepted client sockets use `TCP_NODELAY` to avoid delayed small replies when
large pipelined requests arrive in pieces.
The parser, writer, commands, and database do not use socket types.
By default, the server uses four I/O threads: the main thread accepts connections
and three workers each own an epoll loop and their assigned client sockets.
Workers parse requests, execute commands, and send replies. One shared mutex
protects the database and AOF append, so commands execute one at a time and
each worker preserves reply order for its clients. The main thread runs active
expiry under the same mutex. The optional every-second AOF sync worker is
separate from the I/O workers. Use `--io-threads 1` to run the original single
epoll loop; `--io-threads N` supports 1 through 16 total I/O threads.

Each client has input and output byte buffers. A complete command is parsed,
run against the one shared database, and encoded into the output buffer.
Incomplete commands stay in the input buffer. Replies are sent only when the
socket is writable; a client that has finished sending can still receive its
queued replies before the connection closes.

## Build and run on Linux

CMake configuration and tests are included in the repository. A fresh clone
needs CMake 3.16 or newer and a C++17 compiler to build the server.

From this repository's root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/redis-server
# Or use a different port:
./build/redis-server --port 6380
# Or keep data in a log across restarts:
./build/redis-server --port 6380 --aof build/data.aof
# Or sync pending log writes approximately once per second:
./build/redis-server --port 6380 --aof build/data.aof --aof-fsync everysec
# Or save a snapshot every 30 seconds and rewrite the log from 16 MiB:
./build/redis-server --aof build/data.aof --save-seconds 30 --aof-rewrite-min-size 16777216
# Or use the single-thread comparison mode:
./build/redis-server --port 6380 --io-threads 1
```

By default the server listens on `0.0.0.0:6379`, meaning all IPv4 network
interfaces. `--port` accepts a number from 1 to 65535.
Local clients can use `127.0.0.1`; clients on other devices should use this
Linux machine's network IP address. The server currently has no authentication,
so only expose the selected port on a network you trust. Ctrl+C exits through
cleanup and attempts to sync pending AOF writes before the process exits.
If the selected port is already occupied, stop the process using that port
before starting this one.

## Optional persistence

Pass `--aof path` to replay an existing log at startup and record later writes.
The parent directory must exist. The log stores length-framed RESP commands.
Most writes are recorded as the resulting value of each affected key with its
absolute expiry deadline. Single-key writes to lists, hashes, sets and sorted
sets (and `APPEND`) are recorded as the command itself when the key was live
with no expiry just before the command and has none afterwards: they depend on
nothing but the key's earlier value, so replaying them gives the same result,
and the record stays the size of the command however large the key is.
`SPOP` is recorded as an `SREM` of the members it removed, and an `EXPIRE`-family
command on such a key as just the new deadline (`PEXPIREAT`), `PERSIST`, or `DEL`.
Any other case keeps the whole-value record, which starts with `DEL`: a key with
an expiry could expire before the log is replayed, a new key could have an older
expired value that a replaying machine with a slower clock still sees, and a
short record after an earlier record's deadline would find the key already gone.
An incomplete final record is discarded on restart. This is a custom format,
not a Redis-compatible AOF file.

The default `--aof-fsync everysec` acknowledges after writing the record
and syncs pending writes approximately once per second in a background thread.
It reduces per-write latency but can lose recent acknowledged writes after an
operating-system crash or power failure. `--aof-fsync always` calls `fdatasync`
before acknowledging each logged write. A background sync can still delay a
concurrent write to the same file. Writes that record a whole key, such as
`RENAME`, `SMOVE`, the `…STORE` commands, and writes to a large key with an
expiry, still make records as large as that key.

A snapshot is written to `<aof path>.snapshot` on a clean shutdown, by `SAVE`,
and every `--save-seconds` seconds if at least one write was logged since the
last one. The periodic snapshot is off by default because it blocks every
client while it runs (see below). Each snapshot also appends a checkpoint record to
the AOF. Startup loads a valid snapshot, verifies its checkpoint record in the
AOF, and replays only later records. If the snapshot is missing, invalid, or its
checkpoint record does not match, startup replays the full AOF. Startup fails if
the AOF ends before a valid snapshot's saved checkpoint.

`BGREWRITEAOF` replaces the AOF with one record per live key plus a checkpoint,
and saves a matching snapshot. The rewritten log is complete on its own, so
it still restores everything if the snapshot is lost. The rewrite also runs
automatically once the AOF is at least `--aof-rewrite-min-size` bytes (default
64 MiB) and twice its size at startup or after the last rewrite. The files are
switched so that a crash at any point leaves a state startup can load: the old
snapshot is removed, then the new AOF is renamed into place, then the new snapshot.
`--save-seconds 0` (the default) and `--aof-rewrite-min-size 0` turn either
automatic step off. A crash during a rewrite can leave `<aof path>.rewrite.*` and
`<aof path>.snapshot.tmp.*` files behind; startup deletes them.

Unlike Redis, `SAVE` and `BGREWRITEAOF` run while holding the database lock, so
every client waits until they finish; `BGREWRITEAOF` replies with Redis's
"started" text only after the rewrite is done. A failed scheduled attempt is
logged and retried after 5 seconds. A failure that may leave a partial record in
the live AOF stops the server.
Future mutating commands must be added to the persistence command list. The log
has no checksum, so corruption inside an otherwise valid record may go undetected.

In another terminal, if `redis-cli` is installed:

```sh
redis-cli -h 127.0.0.1 -p 6379 PING
redis-cli -h 127.0.0.1 -p 6379 SET name Kunal
redis-cli -h 127.0.0.1 -p 6379 MSET city Delhi language C++
redis-cli -h 127.0.0.1 -p 6379 MGET name missing name
```

To run the parser and real TCP tests:

```sh
ctest --test-dir build --output-on-failure
```

The TCP tests start their own servers on temporary ports. The test clients are
C++ executables built by CMake; Python is not required for CTest.
The suite also runs socket, AOF restart, AOF rewrite, SIGINT, and concurrent-client tests
with four I/O threads. For pipelined PING, GET, SET, and INCR comparisons, run
`./build/io-threads-bench ./build/redis-server`; it reports throughput,
pipeline round-trip latency, and main-thread CPU use. Pass an optional value
size in bytes, such as `./build/io-threads-bench ./build/redis-server 8`,
and optionally a command name, such as
`./build/io-threads-bench ./build/redis-server 128 SET`.
To scan latency for every implemented command with 128-byte keys and values,
run `./build/command-latency-bench ./build/redis-server 128`. It compares one
and four I/O threads and reports p50, p95, p99, and the number of pipelines
taking at least 20 ms for each command and for all commands combined.
For single-command round trips, run
`./build/command-latency-bench ./build/redis-server 128 1 400`.
Results depend on the machine and workload.

## Optional comparison with Redis 8.2.9

The differential PING test compares raw RESP2 replies from this server and
Redis Open Source 8.2.9. The reference source and binaries live under the
ignored `build/reference/` directory. To prepare the reference server on Linux:

```sh
mkdir -p build/reference
curl -fL https://download.redis.io/releases/redis-8.2.9.tar.gz -o build/reference/redis-8.2.9.tar.gz
echo '531b314e5557ad76d941f605b3e3162ac61dc141f37c407e1f91fcfe17ea8c30  build/reference/redis-8.2.9.tar.gz' | sha256sum -c -
tar -xzf build/reference/redis-8.2.9.tar.gz -C build/reference
make -C build/reference/redis-8.2.9 -j2
```

After building this server with CMake, run:

```sh
./build/differential-ping-test ./build/redis-server build/reference/redis-8.2.9/src/redis-server
```

The same comparison for the admin commands (`TYPE`, `KEYS`, `SCAN`, `RENAME`,
`FLUSHDB`/`FLUSHALL`, `SELECT`, `COMMAND`, `CONFIG GET`, `INFO`) sends the same
commands to both servers and prints every reply that differs:

```sh
./build/differential-admin-test ./build/redis-server build/reference/redis-8.2.9/src/redis-server
```

`./build/differential-types-test` does the same for the string, list, hash and
set commands, including their error replies and the order arguments are checked,
and `./build/differential-zset-test` for the sorted-set commands, including score
formatting and range parsing.

The differential tests are separate from the default `ctest` suite because a
fresh clone does not contain the reference Redis binary.

## Learning checkpoints

1. Read `src/main.cpp`: it starts the server.
2. Read `src/net/server.cpp`: follow accept → receive → parse → execute → send.
3. Read `src/protocol/resp_parser.cpp`: it waits for a whole RESP command and
   records exactly how many bytes were consumed.
4. Read `src/commands/command_handler.cpp`: command names become ASCII
   uppercase, while keys and values keep their original bytes.
5. Read `src/protocol/resp_writer.cpp`: `MGET` forms an array from already
   encoded bulk strings and null bulk strings.
6. Run `ctest`, then try the `redis-cli` commands above.

The parser allows up to 1024 arguments and a 64 KiB bulk string. Each client
has 128 KiB pending-input and pending-output limits, enough for one maximum
bulk string plus RESP framing. The server closes a client that exceeds a
buffer limit or sends invalid RESP. The loop accepts at most 16 connections
and performs at most four successful reads per client event before handling
other ready events. Parallel command execution and full Redis compatibility
are future steps.
