"""Compare one RESP2 PING reply with Redis Open Source 8.2.9."""

import socket
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUR_SERVER = ROOT / "build/redis-server"
REFERENCE_SERVER = ROOT / "build/reference/redis-8.2.9/src/redis-server"
PING_REQUEST = b"*1\r\n$4\r\nPING\r\n"


def available_ports():
    with socket.socket() as first, socket.socket() as second:
        first.bind(("127.0.0.1", 0))
        second.bind(("127.0.0.1", 0))
        return first.getsockname()[1], second.getsockname()[1]


def wait_until_ready(process, port):
    for _ in range(100):
        if process.poll() is not None:
            error = process.stderr.read().decode(errors="replace")
            raise RuntimeError("server exited during startup: " + error)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("server did not start on port " + str(port))


def ping_reply(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(PING_REQUEST)
        reply = bytearray()
        while not reply.endswith(b"\r\n"):
            chunk = connection.recv(1)
            if not chunk:
                raise RuntimeError("connection closed before a complete reply")
            reply.extend(chunk)
        return bytes(reply)


def main():
    our_port, reference_port = available_ports()
    servers = []
    try:
        servers.append(subprocess.Popen(
            [str(OUR_SERVER), "--port", str(our_port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
        servers.append(subprocess.Popen(
            [str(REFERENCE_SERVER), "--bind", "127.0.0.1",
             "--port", str(reference_port), "--save", "", "--appendonly", "no"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))

        wait_until_ready(servers[0], our_port)
        wait_until_ready(servers[1], reference_port)

        our_reply = ping_reply(our_port)
        reference_reply = ping_reply(reference_port)
        assert reference_reply == b"+PONG\r\n", reference_reply
        assert our_reply == reference_reply, (our_reply, reference_reply)
        print("PING reply matches Redis Open Source 8.2.9")
    finally:
        for server in servers:
            if server.poll() is None:
                server.terminate()
        for server in servers:
            try:
                server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            server.stderr.close()


if __name__ == "__main__":
    main()
