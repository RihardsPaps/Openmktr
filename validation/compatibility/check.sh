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
out/load_limit_test
python3 tests/version_generator.py
python3 tests/correctness.py
python3 tests/make_compat_regressions.py
python3 tests/portage.py
python3 tests/load_limits.py
python3 tests/portage_validation.py
python3 tests/ninja_jobserver.py
sh testcase/dump/run.sh
