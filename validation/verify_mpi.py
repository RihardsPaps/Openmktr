"""Compile and run a two-rank MPI program against one installed toolchain."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument("prefix", type=Path)
args = parser.parse_args()
prefix = args.prefix.resolve()
env = os.environ.copy()
env["PATH"] = f"{prefix / 'bin'}:{env['PATH']}"
env["LD_LIBRARY_PATH"] = f"{prefix / 'lib'}:{prefix / 'lib64'}:{env.get('LD_LIBRARY_PATH', '')}"
env["OMPI_ALLOW_RUN_AS_ROOT"] = "1"
env["OMPI_ALLOW_RUN_AS_ROOT_CONFIRM"] = "1"

source = r"""
#include <mpi.h>
#include <stdio.h>
int main(int argc, char **argv) {
  int rank, size, sum;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Allreduce(&rank, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  printf("rank=%d size=%d sum=%d\n", rank, size, sum);
  MPI_Finalize();
  return size == 2 && sum == 1 ? 0 : 1;
}
"""
with tempfile.TemporaryDirectory(prefix="mpi-smoke-") as tmp:
    src = Path(tmp) / "smoke.c"
    binary = Path(tmp) / "smoke"
    src.write_text(source)
    subprocess.run([str(prefix / "bin/mpicc"), str(src), "-o", str(binary)],
                   check=True, env=env)
    run = subprocess.run([str(prefix / "bin/mpirun"), "-n", "2", str(binary)],
                         check=True, capture_output=True, text=True, env=env,
                         timeout=60)
    lines = run.stdout.splitlines()
    assert sorted(lines) == ["rank=0 size=2 sum=1", "rank=1 size=2 sum=1"], lines
    print("\n".join(lines))
