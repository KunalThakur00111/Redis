"""Measure sequential SET round-trip latency with and without the current AOF."""

import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def request():
    value = b"x" * 64
    return (b"*3\r\n$3\r\nSET\r\n$5\r\nbench\r\n$64\r\n" +
            value + b"\r\n")


def read_exact(connection, count):
    result = bytearray()
    while len(result) < count:
        chunk = connection.recv(count - len(result))
        if not chunk:
            raise RuntimeError("server closed before the full reply")
        result.extend(chunk)
    return bytes(result)


def start_server(binary, aof_path, extra_args):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    args = [binary, "--port", str(port)]
    if aof_path is not None:
        args.extend(("--aof", str(aof_path)))
    args.extend(extra_args)
    server = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE)
    for _ in range(100):
        if server.poll() is not None:
            raise RuntimeError("server exited: " +
                               server.stderr.read().decode(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return server, port
        except OSError:
            time.sleep(0.02)
    server.terminate()
    server.communicate(timeout=2)
    raise RuntimeError("server did not start")


def measure(binary, aof_path, count, extra_args):
    server, port = start_server(binary, aof_path, extra_args)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
            connection.settimeout(5)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            payload = request()
            durations_ms = []
            for i in range(count + 20):
                start_ns = time.perf_counter_ns()
                connection.sendall(payload)
                if read_exact(connection, 5) != b"+OK\r\n":
                    raise RuntimeError("unexpected SET reply")
                elapsed_ms = (time.perf_counter_ns() - start_ns) / 1_000_000
                if i >= 20:
                    durations_ms.append(elapsed_ms)
    finally:
        server.terminate()
        try:
            server.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()

    ordered = sorted(durations_ms)
    total_s = sum(durations_ms) / 1000
    return (statistics.median(ordered),
            ordered[max(0, int(0.95 * count) - 1)],
            count / total_s)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("Usage: aof_write_latency.py SERVER_BINARY [COUNT]")
    binary = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) == 3 else 500
    if count <= 0:
        raise SystemExit("COUNT must be positive")

    with tempfile.TemporaryDirectory(dir=Path(binary).resolve().parent) as directory:
        print(f"Workload: {count} sequential SETs, one client, one 64-byte value, "
              "pipeline depth 1")
        print(f"AOF directory: {directory}")
        print("A short everysec run may finish before its first periodic sync.")
        for label, aof_path, extra_args in (
            ("memory", None, ()),
            ("AOF + fdatasync per write", Path(directory) / "always.aof", ()),
            ("AOF + everysec", Path(directory) / "everysec.aof",
             ("--aof-fsync", "everysec"))):
            median_ms, p95_ms, ops_per_second = measure(
                binary, aof_path, count, extra_args)
            print(f"{label}: median={median_ms:.3f} ms, p95={p95_ms:.3f} ms, "
                  f"throughput={ops_per_second:.0f} ops/s")


if __name__ == "__main__":
    main()
