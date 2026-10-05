#!/usr/bin/env python3
"""Run a private Redis server and exercise persisted command semantics."""

import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main() -> None:
    server, cli = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="omktr-redis-") as directory:
        socket = Path(directory) / "redis.sock"
        process = subprocess.Popen(
            [server, "--port", "0", "--unixsocket", str(socket),
             "--save", "", "--appendonly", "no", "--dir", directory],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            for _ in range(100):
                if socket.exists():
                    break
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read())
                time.sleep(0.05)
            else:
                raise TimeoutError("Redis socket was not created")

            def run(*args: str) -> str:
                return subprocess.run(
                    [cli, "-s", str(socket), "--raw", *args],
                    text=True, capture_output=True, check=True,
                ).stdout.strip()

            assert run("PING") == "PONG"
            assert run("SET", "key", "value") == "OK"
            assert run("GET", "key") == "value"
            assert run("INCR", "counter") == "1"
            assert run("INCR", "counter") == "2"
            assert run("XADD", "events", "*", "kind", "smoke")
            assert "kind" in run("XRANGE", "events", "-", "+")
            run("SHUTDOWN", "NOSAVE")
            process.wait(timeout=10)
            assert process.returncode == 0, process.returncode
            print(f"{subprocess.check_output([server, '--version'], text=True).strip()}; "
                  "private-socket commands PASS")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
