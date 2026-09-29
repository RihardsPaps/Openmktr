"""Check the final compiler's C++ library against the bootstrapped glibc."""
from pathlib import Path
import subprocess
import sys


root = Path(sys.argv[1])
prefix, sysroot = root / "clean-toolchain", root / "clean-sysroot"
compiler = prefix / "bin/x86_64-linux-gnu-g++"
assert subprocess.check_output([compiler, "-print-sysroot"], text=True).strip() == str(sysroot)
source = root / "clean-builds/smoke.cpp"
source.write_text('#include <iostream>\n#include <numeric>\n#include <vector>\n'
                  'int main(){std::vector<int> v{1,2,3};std::cout << "bootstrap-cpp-ok:" '
                  '<< std::accumulate(v.begin(),v.end(),0) << "\\n";}\n')
executable = source.with_suffix("")
subprocess.run([compiler, "-std=c++20", "-O2", source, "-o", executable], check=True)
libraries = ":".join(map(str, [prefix / "lib64", sysroot / "lib64",
                             sysroot / "usr/lib64", sysroot / "lib", sysroot / "usr/lib"]))
output = subprocess.check_output([sysroot / "lib64/ld-linux-x86-64.so.2",
                                  "--library-path", libraries, executable], text=True)
assert output == "bootstrap-cpp-ok:6\n", output
print(output, end="")
