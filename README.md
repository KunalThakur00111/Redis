# Redis-compatible learning server

A small, single-threaded C++17 in-memory server. It accepts RESP arrays of bulk
strings over TCP and currently supports `PING`, `ECHO`, `SET`, `GET`, `DEL`,
`EXISTS`, `DBSIZE`, and `MGET`. Data is shared between clients and disappears
when the process exits.

## Architecture

```text
TCP client
   │ RESP bytes
   ▼
net/server.cpp                 select loop, client input/output, socket I/O
   │ complete request                   ▲ encoded reply
   ▼                                    │
protocol/resp_parser.cpp       commands/command_handler.cpp
                                │                  │
                                ▼                  ▼
                       storage/database.h   protocol/resp_writer.cpp
```

The server uses Linux POSIX sockets directly. The parser, writer, commands,
and database do not use socket types, so a later Linux `epoll` loop could reuse
them. The current loop uses `select` so the networking behavior stays easy to
study.

Each client has input and output byte buffers. A complete command is parsed,
run against the one shared database, and encoded into the output buffer.
Incomplete commands stay in the input buffer. Replies are sent only when the
socket is writable; a client that has finished sending can still receive its
queued replies before the connection closes.

## Build and run on Linux

From this repository's root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/redis-server
```

The server listens on `127.0.0.1:6379`. Stop it with Ctrl+C. If port 6379 is
already occupied, stop the process using that port before starting this one.

In another terminal, if `redis-cli` is installed:

```sh
redis-cli -h 127.0.0.1 -p 6379 PING
redis-cli -h 127.0.0.1 -p 6379 SET name Kunal
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
buffer limit or sends invalid RESP. `select` also limits the number of clients
and usable descriptor values. Fairness budgets, persistence, expiry, and full
Redis compatibility are future steps.
