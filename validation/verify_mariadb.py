"""Start an installed MariaDB on a private socket and exercise SQL."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


prefix = Path(sys.argv[1]).resolve()
build = Path(sys.argv[2]).resolve()
root = Path(tempfile.mkdtemp(prefix="mariadb-verify-", dir=build))
data = root / "data"
socket = root / "server.sock"
server_log = root / "server.log"
env = os.environ.copy()
env["PATH"] = f"{prefix / 'bin'}:{env['PATH']}"
env["LD_LIBRARY_PATH"] = f"{prefix / 'lib'}:{env.get('LD_LIBRARY_PATH', '')}"
server = None
try:
    init = subprocess.run(
        [str(prefix / "scripts/mariadb-install-db"), "--no-defaults",
         f"--basedir={prefix}", f"--datadir={data}", "--user=root",
         "--auth-root-authentication-method=normal", "--skip-test-db"],
        check=True, capture_output=True, text=True, timeout=180, env=env)
    with server_log.open("w") as output:
        server = subprocess.Popen(
            [str(prefix / "bin/mariadbd"), "--no-defaults", "--user=root",
             f"--basedir={prefix}", f"--datadir={data}",
             f"--socket={socket}", f"--pid-file={root / 'server.pid'}",
             "--skip-networking"],
            stdout=output, stderr=subprocess.STDOUT, env=env)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError(f"mariadbd exited {server.returncode}")
            if socket.exists():
                probe = subprocess.run(
                    [str(prefix / "bin/mariadb-admin"), "--no-defaults",
                     f"--socket={socket}", "--user=root", "ping"],
                    capture_output=True, text=True, timeout=10, env=env)
                if probe.returncode == 0:
                    break
            time.sleep(0.25)
        else:
            raise RuntimeError("MariaDB did not accept socket connections")
        sql = ("CREATE DATABASE validation; "
               "CREATE TABLE validation.sample(id INT PRIMARY KEY, value VARCHAR(32)); "
               "INSERT INTO validation.sample VALUES(1, 'pinned build'); "
               "SELECT VERSION(); "
               "SELECT value FROM validation.sample WHERE id=1;")
        result = subprocess.run(
            [str(prefix / "bin/mariadb"), "--no-defaults", f"--socket={socket}",
             "--user=root", "--batch", "--skip-column-names", "--execute", sql],
            check=True, capture_output=True, text=True, timeout=30, env=env)
        assert "11.4.5" in result.stdout, result.stdout
        assert "pinned build" in result.stdout, result.stdout
        print(result.stdout, end="")
        print("Private-socket MariaDB server and SQL smoke PASS")
finally:
    if server is not None and server.poll() is None:
        subprocess.run([str(prefix / "bin/mariadb-admin"), "--no-defaults",
                        f"--socket={socket}", "--user=root", "shutdown"],
                       capture_output=True, timeout=30, check=False, env=env)
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.terminate()
            server.wait(timeout=10)
    if server is None or server.returncode == 0:
        shutil.rmtree(root)
    else:
        print(f"MariaDB diagnostic data retained at {root}", file=sys.stderr)
        if server_log.exists():
            print(server_log.read_text(errors="replace")[-4000:], file=sys.stderr)
