"""Smoke-test an installed MySQL server over a private Unix socket."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


prefix = Path(sys.argv[1]).resolve()
root = Path(tempfile.mkdtemp(prefix="mysql-verify-", dir=prefix.parent.parent / "builds" / "mysql"))
data = root / "data"
socket = root / "mysql.sock"
server_log = root / "mysqld.log"
mysqld = prefix / "bin" / "mysqld"
mysql = prefix / "bin" / "mysql"
mysqladmin = prefix / "bin" / "mysqladmin"


def run(*args, timeout=120):
    return subprocess.run(args, text=True, capture_output=True,
                          check=True, timeout=timeout)


server = None
try:
    run(str(mysqld), "--no-defaults", "--initialize-insecure", "--user=root",
        f"--basedir={prefix}", f"--datadir={data}", timeout=180)
    with server_log.open("w") as output:
        server = subprocess.Popen(
            [str(mysqld), "--no-defaults", "--user=root", f"--basedir={prefix}",
             f"--datadir={data}", f"--socket={socket}",
             f"--pid-file={root / 'mysqld.pid'}", "--skip-networking",
             "--mysqlx=OFF"],
            stdout=output, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError(f"mysqld exited {server.returncode}")
            if socket.exists():
                probe = subprocess.run(
                    [str(mysqladmin), "--no-defaults", "--protocol=socket",
                     f"--socket={socket}", "--user=root", "ping"],
                    text=True, capture_output=True, timeout=10,
                )
                if probe.returncode == 0:
                    break
            time.sleep(0.25)
        else:
            raise RuntimeError("mysqld did not accept socket connections")

        sql = ("CREATE DATABASE validation; "
               "CREATE TABLE validation.sample(id INT PRIMARY KEY, value VARCHAR(32)); "
               "INSERT INTO validation.sample VALUES(1, 'pinned build'); "
               "SELECT VERSION(); "
               "SELECT value FROM validation.sample WHERE id=1;")
        result = run(str(mysql), "--no-defaults", "--protocol=socket",
                     f"--socket={socket}", "--user=root", "--batch",
                     "--skip-column-names", "--execute", sql)
        assert "8.4.4" in result.stdout, result.stdout
        assert "pinned build" in result.stdout, result.stdout
        print(result.stdout, end="")
        print("Private-socket MySQL server and SQL smoke PASS")
finally:
    if server is not None and server.poll() is None:
        subprocess.run([str(mysqladmin), "--no-defaults", "--protocol=socket",
                        f"--socket={socket}", "--user=root", "shutdown"],
                       capture_output=True, timeout=30, check=False)
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.terminate()
            server.wait(timeout=10)
    if server is None or server.returncode == 0:
        shutil.rmtree(root)
    else:
        print(f"MySQL diagnostic data retained at {root}", file=sys.stderr)
        if server_log.exists():
            print(server_log.read_text(errors="replace")[-4000:], file=sys.stderr)
