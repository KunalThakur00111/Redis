"""Check that SIGINT exits cleanly and AOF data survives an immediate restart."""

import signal
import sys
import tempfile
from pathlib import Path

from aof_restart_test import exchange, start_server, stop_server


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        aof_path = Path(directory) / "data.aof"
        server, port = start_server(binary, aof_path)
        try:
            assert exchange(port, "SET", "name", "Kunal") == b"+OK\r\n"
            server.send_signal(signal.SIGINT)
            _, stderr = server.communicate(timeout=5)
            assert server.returncode == 0, stderr.decode(errors="replace")
        finally:
            if server.poll() is None:
                stop_server(server)

        server, port = start_server(binary, aof_path)
        try:
            assert exchange(port, "GET", "name") == b"$5\r\nKunal\r\n"
        finally:
            stop_server(server)

    print("AOF SIGINT restart test passed")


if __name__ == "__main__":
    main()
