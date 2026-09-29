"""Smoke-test the installed OpenBLAS CBLAS and LAPACKE interfaces."""
from pathlib import Path
import subprocess
import sys
import tempfile


prefix = Path(sys.argv[1])
source = r"""
#include <math.h>
#include <cblas.h>
#include <lapacke.h>
int main(void) {
  const double identity[4] = {1, 0, 0, 1};
  double product[4] = {0, 0, 0, 0};
  cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 2, 2, 2,
              1.0, identity, 2, identity, 2, 0.0, product, 2);
  for (int i = 0; i < 4; ++i)
    if (fabs(product[i] - identity[i]) > 1e-12) return 1;
  double a[4] = {3, 1, 1, 2}, b[2] = {9, 8};
  lapack_int piv[2];
  if (LAPACKE_dgesv(LAPACK_ROW_MAJOR, 2, 1, a, 2, piv, b, 1)) return 2;
  if (fabs(b[0] - 2) > 1e-12 || fabs(b[1] - 3) > 1e-12) return 3;
  return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="openblas-smoke-") as temp:
    path = Path(temp)
    (path / "smoke.c").write_text(source)
    subprocess.run(
        ["cc", str(path / "smoke.c"), "-I", str(prefix / "include"),
         "-L", str(prefix / "lib"),
         f"-Wl,-rpath,{prefix / 'lib'}", "-lopenblas", "-lm", "-pthread",
         "-o", str(path / "smoke")],
        check=True,
    )
    subprocess.run([str(path / "smoke")], check=True)
print("OpenBLAS CBLAS dgemm and LAPACKE dgesv smoke PASS")
