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
    server = subprocess.Popen([sys.argv[1]], stdout=subprocess.DEVNULL,
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

        pipeline = (request("SET", "key", "first") +
                    request("SET", "empty", "") +
                    request("SET", "key", "second") +
                    request("DBSIZE"))
        assert exchange(pipeline) == b"+OK\r\n+OK\r\n+OK\r\n:2\r\n"

        expected = b"*4\r\n$6\r\nsecond\r\n$-1\r\n$0\r\n\r\n$6\r\nsecond\r\n"
        assert exchange(request("mget", "key", "missing", "empty", "key")) == expected
        assert exchange(request("DBSIZE")) == b":2\r\n"
        assert exchange(request("EXISTS", "key", "key", "missing")) == b":2\r\n"
        assert exchange(request("DEL", "key", "key", "missing")) == b":1\r\n"
        assert exchange(request("GET", "key")) == b"$-1\r\n"
        assert exchange(request("MGET")) == (
            b"-ERR wrong number of arguments for 'MGET' command\r\n")

        large = b"x" * 65536
        assert exchange(request("SET", "large", large)) == b"+OK\r\n"
        assert exchange(request("GET", "large")) == (
            b"$65536\r\n" + large + b"\r\n")
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
