# Redis-compatible server (in progress)

A Linux-only, single-threaded C++17 in-memory server. It accepts RESP arrays of bulk
strings over TCP and currently supports `PING`, `ECHO`, `SET`, `MSET`, `GET`, `DEL`,
`EXISTS`, `DBSIZE`, `MGET`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`,
`TTL`, `PTTL`, `PERSIST`, `EXPIRETIME`, `PEXPIRETIME`, `GETDEL`, `INCR`, `DECR`,
`INCRBY`, `DECRBY`, `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN`,
`LINDEX`, `HSET`, `HGET`, `HDEL`, `HLEN`, `SADD`, `SREM`, `SISMEMBER`,
`SCARD`, and `SMEMBERS`.
`SET` supports `NX`, `XX`, `EX`, `PX`, `EXAT`, and `PXAT`, including a condition
with an expiry.
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

The server uses Linux POSIX sockets and a level-triggered `epoll` loop.
The parser, writer, commands, and database do not use socket types.
The event loop registers the listener and each client with epoll, then waits
for readable or writable sockets.

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
```

By default the server listens on `0.0.0.0:6379`, meaning all IPv4 network
interfaces. `--port` accepts a number from 1 to 65535.
Local clients can use `127.0.0.1`; clients on other devices should use this
Linux machine's network IP address. The server currently has no authentication,
so only expose the selected port on a network you trust. Stop it with Ctrl+C.
If the selected port is already occupied, stop the process using that port
before starting this one.

## Optional persistence

Pass `--aof path` to replay an existing log at startup and record later writes.
The parent directory must exist. The log stores length-framed RESP commands
describing each affected key's resulting value and absolute expiry deadline.
An incomplete final record is discarded on restart. This is a custom format,
not a Redis-compatible AOF file.

The default `--aof-fsync always` calls `fdatasync` before acknowledging each
logged write. `--aof-fsync everysec` acknowledges after writing the record
and syncs pending writes approximately once per second in the event loop.
That mode reduces per-write latency but can lose recent acknowledged writes
after an operating-system crash or power failure; its periodic sync can still
pause the event loop. The log records a whole affected key after a write and
does not compact old records, so large values and long-running workloads can
make writes and startup slow.
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

The TCP test starts its own server on a temporary port. Python 3 is needed for
that test; CMake skips it if Python is not found.

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
python3 tests/differential_ping_test.py
```

The differential test is separate from the default `ctest` suite because a
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
other ready events. Multithreading and full Redis
compatibility are future steps.
