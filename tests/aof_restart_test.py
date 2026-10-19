"""Check that the optional append-only log restores data after a restart."""

import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def request(*arguments):
    parts = [b"*" + str(len(arguments)).encode() + b"\r\n"]
    for argument in arguments:
        if isinstance(argument, str):
            argument = argument.encode()
        parts.extend((b"$" + str(len(argument)).encode() + b"\r\n",
                      argument, b"\r\n"))
    return b"".join(parts)


def start_server(binary, aof_path):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    server = subprocess.Popen(
        [binary, "--port", str(port), "--aof", str(aof_path)] + sys.argv[2:],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        if server.poll() is not None:
            raise RuntimeError("server exited during startup: " +
                               server.stderr.read().decode(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return server, port
        except OSError:
            time.sleep(0.02)
    server.terminate()
    server.communicate(timeout=2)
    raise RuntimeError("server did not start")


def stop_server(server):
    server.terminate()
    try:
        server.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        server.kill()
        server.communicate()


def exchange(port, *arguments):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(request(*arguments))
        connection.shutdown(socket.SHUT_WR)
        reply = bytearray()
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                return bytes(reply)
            reply.extend(chunk)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        aof_path = Path(directory) / "data.aof"
        server, port = start_server(binary, aof_path)
        try:
            assert exchange(port, "SET", "text", b"a\x00b") == b"+OK\r\n"
            assert exchange(port, "MSET", "first", "one", "second", "two") == b"+OK\r\n"
            assert exchange(port, "DEL", "second") == b":1\r\n"
            assert exchange(port, "RPUSH", "items", "a", "b") == b":2\r\n"
            assert exchange(port, "HSET", "profile", "name", "Kunal") == b":1\r\n"
            assert exchange(port, "SADD", "members", "x", "y") == b":2\r\n"
            assert exchange(port, "SET", "short", "gone", "PX", "150") == b"+OK\r\n"
            assert exchange(port, "PEXPIRE", "items", "60000") == b":1\r\n"
        finally:
            stop_server(server)

        complete_size = aof_path.stat().st_size
        with aof_path.open("ab") as log:
            log.write(struct.pack("<Q", 20) + b"partial")
        time.sleep(0.2)

        server, port = start_server(binary, aof_path)
        try:
            assert aof_path.stat().st_size == complete_size
            assert exchange(port, "GET", "text") == b"$3\r\na\x00b\r\n"
            assert exchange(port, "GET", "first") == b"$3\r\none\r\n"
            assert exchange(port, "GET", "second") == b"$-1\r\n"
            assert exchange(port, "LRANGE", "items", "0", "-1") == (
                b"*2\r\n$1\r\na\r\n$1\r\nb\r\n")
            assert int(exchange(port, "PTTL", "items")[1:-2]) > 0
            assert exchange(port, "HGET", "profile", "name") == b"$5\r\nKunal\r\n"
            assert exchange(port, "SISMEMBER", "members", "x") == b":1\r\n"
            assert exchange(port, "SISMEMBER", "members", "y") == b":1\r\n"
            assert exchange(port, "GET", "short") == b"$-1\r\n"
            assert exchange(port, "INCR", "counter") == b":1\r\n"
        finally:
            stop_server(server)

        server, port = start_server(binary, aof_path)
        try:
            assert exchange(port, "GET", "counter") == b"$1\r\n1\r\n"
        finally:
            stop_server(server)

    print("AOF restart test passed")


if __name__ == "__main__":
    main()
