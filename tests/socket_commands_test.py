"""Small end-to-end test of the server over a real TCP connection."""

import socket
import subprocess
import sys
import time


ADDRESS = ("127.0.0.1", 6379)


def request(*arguments):
    parts = [b"*" + str(len(arguments)).encode() + b"\r\n"]
    for argument in arguments:
        if isinstance(argument, str):
            argument = argument.encode()
        parts.extend((b"$" + str(len(argument)).encode() + b"\r\n",
                      argument, b"\r\n"))
    return b"".join(parts)


def read_exact(connection, count):
    reply = bytearray()
    while len(reply) < count:
        chunk = connection.recv(count - len(reply))
        if not chunk:
            raise RuntimeError("connection closed before the full reply arrived")
        reply.extend(chunk)
    return bytes(reply)


def exchange(payload, chunks=None):
    with socket.create_connection(ADDRESS, timeout=2) as connection:
        connection.settimeout(2)
        if chunks is None:
            connection.sendall(payload)
        else:
            for chunk in chunks:
                connection.sendall(chunk)
        connection.shutdown(socket.SHUT_WR)
        reply = bytearray()
        while True:
            data = connection.recv(65536)
            if not data:
                return bytes(reply)
            reply.extend(data)


def main():
    global ADDRESS
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        ADDRESS = ("127.0.0.1", probe.getsockname()[1])

    server = subprocess.Popen([sys.argv[1], "--port", str(ADDRESS[1])],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            if server.poll() is not None:
                raise RuntimeError("server exited during startup: " +
                                   server.stderr.read().decode(errors="replace"))
            try:
                with socket.create_connection(ADDRESS, timeout=0.1):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise RuntimeError("server did not start")

        assert exchange(request("PING")) == b"+PONG\r\n"
        assert exchange(request("ping", b"a\x00b")) == b"$3\r\na\x00b\r\n"
        echo = request("ECHO", b"a\r\nb")
        assert exchange(echo, [echo[:7], echo[7:]]) == b"$4\r\na\r\nb\r\n"

        # One client's incomplete request must not block another ready client.
        with socket.create_connection(ADDRESS, timeout=2) as first, \
             socket.create_connection(ADDRESS, timeout=2) as second:
            first.settimeout(2)
            second.settimeout(2)
            ping = request("PING")
            first.sendall(ping[:5])
            second.sendall(ping)
            assert read_exact(second, 7) == b"+PONG\r\n"
            first.sendall(ping[5:])
            assert read_exact(first, 7) == b"+PONG\r\n"

        pipeline = (request("SET", "key", "first") +
                    request("SET", "empty", "") +
                    request("SET", "key", "second") +
                    request("DBSIZE"))
        assert exchange(pipeline) == b"+OK\r\n+OK\r\n+OK\r\n:2\r\n"

        malformed = b"*1\r\n$x\r\nPING\r\n"
        for payload, prefix, line_count in (
            (malformed, b"-ERR Protocol error: ", 1),
            (request("PING") + malformed, b"+PONG\r\n-ERR Protocol error: ", 2),
        ):
            with socket.create_connection(ADDRESS, timeout=2) as connection:
                connection.settimeout(2)
                connection.sendall(payload)
                reply = bytearray()
                while True:
                    data = connection.recv(65536)
                    if not data:
                        break
                    reply.extend(data)
                assert reply.startswith(prefix)
                assert reply.endswith(b"\r\n")
                assert reply.count(b"\r\n") == line_count

        expected = b"*4\r\n$6\r\nsecond\r\n$-1\r\n$0\r\n\r\n$6\r\nsecond\r\n"
        assert exchange(request("mget", "key", "missing", "empty", "key")) == expected
        assert exchange(request("DBSIZE")) == b":2\r\n"
        assert exchange(request("EXISTS", "key", "key", "missing")) == b":2\r\n"
        assert exchange(request("DEL", "key", "key", "missing")) == b":1\r\n"
        assert exchange(request("GET", "key")) == b"$-1\r\n"
        assert exchange(request("MGET")) == (
            b"-ERR wrong number of arguments for 'mget' command\r\n")

        assert exchange(request("MSET", "first", "one", "second", b"a\x00b",
                                "first", "updated")) == b"+OK\r\n"
        assert exchange(request("MGET", "first", "second")) == (
            b"*2\r\n$7\r\nupdated\r\n$3\r\na\x00b\r\n")
        assert exchange(request("MSET", "orphan")) == (
            b"-ERR wrong number of arguments for 'mset' command\r\n")
        assert exchange(request("MSET", "untouched", "value", "orphan")) == (
            b"-ERR wrong number of arguments for 'mset' command\r\n")
        assert exchange(request("GET", "untouched")) == b"$-1\r\n"
        assert exchange(request("GET", "orphan")) == b"$-1\r\n"

        large = b"x" * 65536
        assert exchange(request("SET", "large", large)) == b"+OK\r\n"
        assert exchange(request("GET", "large")) == (
            b"$65536\r\n" + large + b"\r\n")

        assert exchange(request("INCR", "counter")) == b":1\r\n"
        assert exchange(request("DECR", "counter")) == b":0\r\n"
        assert exchange(request("DECR", "missing-counter")) == b":-1\r\n"
        assert exchange(request("DECR")) == (
            b"-ERR wrong number of arguments for 'decr' command\r\n")

        assert exchange(request("SET", "signed", "+7")) == b"+OK\r\n"
        assert exchange(request("INCR", "signed")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("DECR", "signed")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "signed")) == b"$2\r\n+7\r\n"

        assert exchange(request("SET", "negative", "-7")) == b"+OK\r\n"
        assert exchange(request("INCR", "negative")) == b":-6\r\n"

        assert exchange(request("SET", "leading-zero", "01")) == b"+OK\r\n"
        assert exchange(request("INCR", "leading-zero")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "leading-zero")) == b"$2\r\n01\r\n"

        assert exchange(request("SET", "negative-zero", "-0")) == b"+OK\r\n"
        assert exchange(request("DECR", "negative-zero")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "negative-zero")) == b"$2\r\n-0\r\n"

        minimum = "-9223372036854775808"
        assert exchange(request("SET", "minimum", minimum)) == b"+OK\r\n"
        assert exchange(request("DECR", "minimum")) == (
            b"-ERR increment or decrement would overflow\r\n")
        assert exchange(request("GET", "minimum")) == (
            b"$20\r\n-9223372036854775808\r\n")

        assert exchange(request("SET", "ttl-counter", "5", "PX", "60000")) == b"+OK\r\n"
        assert exchange(request("DECR", "ttl-counter")) == b":4\r\n"
        remaining = exchange(request("PTTL", "ttl-counter"))
        assert remaining.startswith(b":") and int(remaining[1:-2]) > 0

        assert exchange(request("INCRBY", "incrby-missing", "5")) == b":5\r\n"
        assert exchange(request("INCRBY", "incrby-missing", "-2")) == b":3\r\n"
        assert exchange(request("GET", "incrby-missing")) == b"$1\r\n3\r\n"
        assert exchange(request("INCRBY", "incrby-negative", "-2")) == b":-2\r\n"
        assert exchange(request("INCRBY", "incrby-missing", "+7")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "incrby-missing")) == b"$1\r\n3\r\n"

        assert exchange(request("SET", "incrby-bad", "abc")) == b"+OK\r\n"
        assert exchange(request("INCRBY", "incrby-bad", "1")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "incrby-bad")) == b"$3\r\nabc\r\n"

        assert exchange(request("SET", "incrby-max", "9223372036854775807")) == b"+OK\r\n"
        assert exchange(request("INCRBY", "incrby-max", "1")) == (
            b"-ERR increment or decrement would overflow\r\n")
        assert exchange(request("GET", "incrby-max")) == (
            b"$19\r\n9223372036854775807\r\n")
        assert exchange(request("SET", "incrby-min", minimum)) == b"+OK\r\n"
        assert exchange(request("INCRBY", "incrby-min", "-1")) == (
            b"-ERR increment or decrement would overflow\r\n")
        assert exchange(request("GET", "incrby-min")) == (
            b"$20\r\n-9223372036854775808\r\n")

        assert exchange(request("SET", "incrby-ttl", "5", "PX", "60000")) == b"+OK\r\n"
        assert exchange(request("INCRBY", "incrby-ttl", "2")) == b":7\r\n"
        remaining = exchange(request("PTTL", "incrby-ttl"))
        assert remaining.startswith(b":") and int(remaining[1:-2]) > 0
        assert exchange(request("INCRBY", "incrby-ttl")) == (
            b"-ERR wrong number of arguments for 'incrby' command\r\n")

        assert exchange(request("DECRBY", "decrby-missing", "3")) == b":-3\r\n"
        assert exchange(request("DECRBY", "decrby-negative", "-3")) == b":3\r\n"
        assert exchange(request("GET", "decrby-negative")) == b"$1\r\n3\r\n"
        assert exchange(request("DECRBY", "decrby-negative", "-3")) == b":6\r\n"

        assert exchange(request("DECRBY", "decrby-min-amount", minimum)) == (
            b"-ERR decrement would overflow\r\n")
        assert exchange(request("GET", "decrby-min-amount")) == b"$-1\r\n"
        assert exchange(request("DECRBY", "decrby-negative", minimum)) == (
            b"-ERR decrement would overflow\r\n")
        assert exchange(request("GET", "decrby-negative")) == b"$1\r\n6\r\n"

        assert exchange(request("SET", "decrby-min", minimum)) == b"+OK\r\n"
        assert exchange(request("DECRBY", "decrby-min", "1")) == (
            b"-ERR increment or decrement would overflow\r\n")
        assert exchange(request("GET", "decrby-min")) == (
            b"$20\r\n-9223372036854775808\r\n")
        assert exchange(request("SET", "decrby-max", "9223372036854775807")) == b"+OK\r\n"
        assert exchange(request("DECRBY", "decrby-max", "-1")) == (
            b"-ERR increment or decrement would overflow\r\n")
        assert exchange(request("GET", "decrby-max")) == (
            b"$19\r\n9223372036854775807\r\n")

        assert exchange(request("DECRBY", "decrby-negative", "+7")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("SET", "decrby-bad", "abc")) == b"+OK\r\n"
        assert exchange(request("DECRBY", "decrby-bad", "1")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "decrby-bad")) == b"$3\r\nabc\r\n"

        assert exchange(request("SET", "decrby-ttl", "5", "PX", "60000")) == b"+OK\r\n"
        assert exchange(request("DECRBY", "decrby-ttl", "2")) == b":3\r\n"
        remaining = exchange(request("PTTL", "decrby-ttl"))
        assert remaining.startswith(b":") and int(remaining[1:-2]) > 0
        assert exchange(request("DECRBY", "decrby-ttl")) == (
            b"-ERR wrong number of arguments for 'decrby' command\r\n")

        assert exchange(request("LPUSH", "list-items", "a", "b", "c")) == b":3\r\n"
        assert exchange(request("LPUSH", "list-items", "d")) == b":4\r\n"
        assert exchange(request("LRANGE", "list-items", "0", "-1")) == (
            b"*4\r\n$1\r\nd\r\n$1\r\nc\r\n$1\r\nb\r\n$1\r\na\r\n")
        assert exchange(request("LRANGE", "list-items", "-2", "-1")) == (
            b"*2\r\n$1\r\nb\r\n$1\r\na\r\n")
        assert exchange(request("LRANGE", "list-items", "10", "20")) == b"*0\r\n"
        assert exchange(request("LRANGE", "missing-list", "0", "-1")) == b"*0\r\n"
        assert exchange(request("LRANGE", "list-items", "01", "-1")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("GET", "list-items")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("MGET", "list-items", "missing")) == (
            b"*2\r\n$-1\r\n$-1\r\n")
        assert exchange(request("GETDEL", "list-items")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("EXISTS", "list-items")) == b":1\r\n"
        assert exchange(request("SET", "string-value", "original")) == b"+OK\r\n"
        assert exchange(request("LRANGE", "string-value", "0", "-1")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("LPUSH", "string-value", "x")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("GET", "string-value")) == b"$8\r\noriginal\r\n"
        assert exchange(request("LPUSH", "list-items")) == (
            b"-ERR wrong number of arguments for 'lpush' command\r\n")

        assert exchange(request("RPUSH", "queue", "a", "b", "c")) == b":3\r\n"
        assert exchange(request("LRANGE", "queue", "0", "-1")) == (
            b"*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n")
        assert exchange(request("LINDEX", "queue", "0")) == b"$1\r\na\r\n"
        assert exchange(request("LINDEX", "queue", "-1")) == b"$1\r\nc\r\n"
        assert exchange(request("LINDEX", "queue", "3")) == b"$-1\r\n"
        assert exchange(request("LINDEX", "missing-list", "0")) == b"$-1\r\n"
        assert exchange(request("LLEN", "queue")) == b":3\r\n"
        assert exchange(request("LLEN", "missing-list")) == b":0\r\n"
        assert exchange(request("LPOP", "queue")) == b"$1\r\na\r\n"
        assert exchange(request("LPOP", "queue", "0")) == b"*0\r\n"
        assert exchange(request("LLEN", "queue")) == b":2\r\n"
        assert exchange(request("RPOP", "queue", "2")) == (
            b"*2\r\n$1\r\nc\r\n$1\r\nb\r\n")
        assert exchange(request("EXISTS", "queue")) == b":0\r\n"
        assert exchange(request("RPOP", "queue")) == b"$-1\r\n"
        assert exchange(request("LPOP", "queue", "2")) == b"*-1\r\n"
        assert exchange(request("LPOP", "queue", "-1")) == (
            b"-ERR value is out of range, must be positive\r\n")
        assert exchange(request("RPOP", "queue", "bad")) == (
            b"-ERR value is not an integer or out of range\r\n")
        assert exchange(request("RPUSH", "string-value", "x")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("LPOP", "string-value")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("RPOP", "string-value", "0")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("LLEN", "string-value")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("LINDEX", "string-value", "0")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

        assert exchange(request("RPUSH", "expiring-list", "a", "b")) == b":2\r\n"
        assert exchange(request("PEXPIRE", "expiring-list", "60000")) == b":1\r\n"
        assert exchange(request("LPOP", "expiring-list")) == b"$1\r\na\r\n"
        remaining = exchange(request("PTTL", "expiring-list"))
        assert remaining.startswith(b":") and int(remaining[1:-2]) > 0
        assert exchange(request("RPOP", "expiring-list")) == b"$1\r\nb\r\n"
        assert exchange(request("PTTL", "expiring-list")) == b":-2\r\n"

        assert exchange(request("HSET", "profile", "name", "Kunal", "place", "Delhi")) == b":2\r\n"
        assert exchange(request("HSET", "profile", "name", "Sam", "age", "21", "age", "22")) == b":1\r\n"
        assert exchange(request("HGET", "profile", "name")) == b"$3\r\nSam\r\n"
        assert exchange(request("HGET", "profile", "age")) == b"$2\r\n22\r\n"
        assert exchange(request("HGET", "profile", "missing")) == b"$-1\r\n"
        assert exchange(request("HLEN", "profile")) == b":3\r\n"
        assert exchange(request("HDEL", "profile", "name", "name", "missing")) == b":1\r\n"
        assert exchange(request("HLEN", "profile")) == b":2\r\n"
        assert exchange(request("HDEL", "profile", "place", "age")) == b":2\r\n"
        assert exchange(request("EXISTS", "profile")) == b":0\r\n"
        assert exchange(request("HGET", "profile", "name")) == b"$-1\r\n"
        assert exchange(request("HLEN", "profile")) == b":0\r\n"
        assert exchange(request("HSET", "broken-hash", "field")) == (
            b"-ERR wrong number of arguments for 'hset' command\r\n")
        assert exchange(request("EXISTS", "broken-hash")) == b":0\r\n"
        assert exchange(request("HSET", "string-value", "field", "value")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("HGET", "list-items", "field")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("HDEL", "list-items", "field")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        assert exchange(request("HLEN", "list-items")) == (
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

        assert exchange(request("HSET", "binary-hash", b"f\x00", b"a\x00b")) == b":1\r\n"
        assert exchange(request("HGET", "binary-hash", b"f\x00")) == b"$3\r\na\x00b\r\n"
        assert exchange(request("HDEL", "binary-hash", b"f\x00")) == b":1\r\n"
        assert exchange(request("EXISTS", "binary-hash")) == b":0\r\n"

        assert exchange(request("HSET", "expiring-hash", "first", "a")) == b":1\r\n"
        assert exchange(request("PEXPIRE", "expiring-hash", "60000")) == b":1\r\n"
        assert exchange(request("HSET", "expiring-hash", "second", "b")) == b":1\r\n"
        remaining = exchange(request("PTTL", "expiring-hash"))
        assert remaining.startswith(b":") and int(remaining[1:-2]) > 0
        assert exchange(request("HDEL", "expiring-hash", "first", "second")) == b":2\r\n"
        assert exchange(request("PTTL", "expiring-hash")) == b":-2\r\n"
        print("Socket command tests passed")
    finally:
        server.terminate()
        try:
            server.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()


if __name__ == "__main__":
    main()
