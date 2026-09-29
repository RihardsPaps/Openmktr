"""Run pgvector in the pinned PostgreSQL server under an unprivileged user."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="pgvector-campaign-") as temporary:
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
            "-o", f'-F -k {socket} -h "" -p 65440', "-w", "start")
        started = True
        result = run("psql", "-h", str(socket), "-p", "65440", "-d", "postgres",
                     "-v", "ON_ERROR_STOP=1", "-Atc",
                     "CREATE EXTENSION vector; "
                     "CREATE TABLE items(id int PRIMARY KEY, embedding vector(3)); "
                     "INSERT INTO items VALUES(1,'[1,2,3]'),(2,'[2,3,4]'); "
                     "CREATE INDEX ON items USING hnsw (embedding vector_l2_ops); "
                     "SELECT extversion FROM pg_extension WHERE extname='vector'; "
                     "SELECT id FROM items ORDER BY embedding <-> '[1,2,3]'::vector LIMIT 2;")
        lines = result.stdout.splitlines()
        assert lines[-3:] == ["0.8.6", "1", "2"], lines
        print("pgvector 0.8.6 extension, HNSW index, and distance order PASS")
    finally:
        if started:
            run("pg_ctl", "-D", str(data), "-m", "fast", "-w", "stop")
