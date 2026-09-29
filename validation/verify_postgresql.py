"""Run the installed PostgreSQL server and contrib extensions as nobody."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="postgresql-campaign-") as temporary:
    root = Path(temporary)
    prefix = root / "install"
    shutil.copytree(sys.argv[1], prefix, symlinks=True)
    socket = root / "socket"
    socket.mkdir()
    for path in [root, *root.rglob("*")]:
        os.chown(path, 65534, 65534, follow_symlinks=False)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(prefix / "lib")
    def run(name, *args):
        return subprocess.run(["runuser", "-u", "nobody", "--",
                               str(prefix / "bin" / name), *args],
                              cwd=root, env=env, text=True,
                              capture_output=True, check=True, timeout=60)
    data = root / "data"
    run("initdb", "-D", str(data), "-A", "trust", "--no-locale")
    started = False
    try:
        run("pg_ctl", "-D", str(data), "-l", str(root / "server.log"),
            "-o", f'-F -k {socket} -h "" -p 65439', "-w", "start")
        started = True
        result = run("psql", "-h", str(socket), "-p", "65439", "-d", "postgres",
                     "-v", "ON_ERROR_STOP=1", "-Atc",
                     "CREATE EXTENSION pg_trgm; CREATE EXTENSION hstore; "
                     "CREATE TABLE verification(id int PRIMARY KEY, value text); "
                     "INSERT INTO verification VALUES(1,'pinned build'); "
                     "SELECT version(); SELECT value FROM verification WHERE id=1; "
                     "SELECT similarity('same','same'), 'key=>value'::hstore -> 'key';")
        assert "PostgreSQL 17.4" in result.stdout, result.stdout
        assert "pinned build" in result.stdout, result.stdout
        assert "1|value" in result.stdout, result.stdout
        print(result.stdout)
        print("Unprivileged server, SQL and pg_trgm/hstore smoke PASS")
    finally:
        if started:
            run("pg_ctl", "-D", str(data), "-m", "fast", "-w", "stop")
