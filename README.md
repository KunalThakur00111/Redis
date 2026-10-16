# Redis-compatible server (in progress)

A small, single-threaded C++17 in-memory server. It accepts RESP arrays of bulk
strings over TCP and currently supports `PING`, `ECHO`, `SET`, `MSET`, `GET`, `DEL`,
`EXISTS`, `DBSIZE`, `MGET`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`,
`TTL`, `PTTL`, `PERSIST`, `EXPIRETIME`, and `PEXPIRETIME`. `SET` supports
`NX`, `XX`, `EX`, `PX`, `EXAT`, and `PXAT`, including a condition with an expiry.
Data is shared between clients and disappears when the process exits.

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

The CMake configuration and tests are kept in this local working copy and are
excluded from Git. A fresh clone needs those files before these commands work.

From this repository's root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/redis-server
# Or use a different port:
./build/redis-server --port 6380
```

By default the server listens on `0.0.0.0:6379`, meaning all IPv4 network
interfaces. `--port` accepts a number from 1 to 65535.
Local clients can use `127.0.0.1`; clients on other devices should use this
Linux machine's network IP address. The server currently has no authentication,
so only expose the selected port on a network you trust. Stop it with Ctrl+C.
If the selected port is already occupied, stop the process using that port
before starting this one.

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

The TCP test starts its own server on port 6379, so stop a manually running
copy first. Python 3 is needed for that test; CMake skips it if Python is not
found.

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
other ready events. Persistence, multithreading, and full Redis
compatibility are future steps.
