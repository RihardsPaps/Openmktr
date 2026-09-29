"""Load installed PostGIS extensions in unprivileged PostgreSQL."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="postgis-campaign-") as temporary:
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
                              capture_output=True, check=True, timeout=90)

    data = root / "data"
    run("initdb", "-D", str(data), "-A", "trust", "--no-locale")
    started = False
    try:
        run("pg_ctl", "-D", str(data), "-l", str(root / "server.log"),
            "-o", f'-F -k {socket} -h "" -p 65441', "-w", "start")
        started = True
        result = run("psql", "-h", str(socket), "-p", "65441", "-d", "postgres",
                     "-v", "ON_ERROR_STOP=1", "-Atc",
                     "CREATE EXTENSION postgis; "
                     "CREATE EXTENSION postgis_raster; "
                     "CREATE EXTENSION postgis_topology; "
                     "SELECT extname FROM pg_extension WHERE extname LIKE 'postgis%' ORDER BY 1; "
                     "SELECT ST_Distance(ST_GeomFromText('POINT(0 0)'), "
                     "ST_GeomFromText('POINT(3 4)'));")
        lines = result.stdout.splitlines()
        assert lines[-4:] == ["postgis", "postgis_raster", "postgis_topology", "5"], lines
        print("PostGIS geometry/raster/topology extensions and distance SQL PASS")
    finally:
        if started:
            run("pg_ctl", "-D", str(data), "-m", "fast", "-w", "stop")
