#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
set -eu
clang++ --version
ninja --version
python3 --version
ldd ./omktr
out/find_test
out/ninja_test
out/strutil_test
python3 tests/version_generator.py
python3 tests/correctness.py
python3 tests/make_compat_regressions.py
python3 tests/ninja_jobserver.py
sh testcase/dump/run.sh
