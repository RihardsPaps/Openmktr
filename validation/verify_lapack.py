"""Smoke-test the pinned LAPACK static libraries with a real solve and GEMM."""
from pathlib import Path
import subprocess
import sys
import tempfile


build = Path(sys.argv[1])
source = r"""
#include <math.h>
extern void dgesv_(const int *, const int *, double *, const int *,
                   int *, double *, const int *, int *);
extern void dgemm_(const char *, const char *, const int *, const int *,
                   const int *, const double *, const double *, const int *,
                   const double *, const int *, const double *, double *,
                   const int *);
int main(void) {
  const int n = 2, one = 1;
  int piv[2], info = -1;
  double a[4] = {3, 1, 1, 2}, b[2] = {9, 8};
  dgesv_(&n, &one, a, &n, piv, b, &n, &info);
  if (info || fabs(b[0] - 2) > 1e-12 || fabs(b[1] - 3) > 1e-12)
    return 1;
  const char trans = 'N';
  const double alpha = 1, beta = 0;
  double identity[4] = {1, 0, 0, 1}, product[4] = {0, 0, 0, 0};
  dgemm_(&trans, &trans, &n, &n, &n, &alpha, identity, &n,
         identity, &n, &beta, product, &n);
  for (int i = 0; i < 4; ++i)
    if (fabs(product[i] - identity[i]) > 1e-12) return 2;
  return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="lapack-smoke-") as temp:
    path = Path(temp)
    (path / "smoke.c").write_text(source)
    subprocess.run(
        ["gfortran-14", str(path / "smoke.c"), str(build / "liblapack.a"),
         str(build / "librefblas.a"), "-lm", "-o", str(path / "smoke")],
        check=True,
    )
    subprocess.run([str(path / "smoke")], check=True)
print("LAPACK dgesv and BLAS dgemm smoke PASS")
