# Contribute to Openmktr

Keep changes focused, preserve Makefile behavior, and verify them in the
project's GNU-free Chimera environment or an equivalent toolchain. Report
issues and submit pull requests in
[the project
repository](https://github.com/RihardsPaps/Openmktr).
Automated agents must also follow [AGENTS.md](AGENTS.md).

## Before changing code

Read the [README](README.md) for build modes and compatibility limits, and
follow the [licensing guide](docs/LICENSING.md). Original fork contributions
use Mozilla Public License 2.0. Preserve upstream copyright and license notices;
add or update a prominent fork change notice in modified upstream files.
Keep parser-sensitive fixture bytes and diagnostic line numbers intact.

The project-controlled build, tests, and runtime must remain GNU-free: use
Clang/LLVM, musl, libc++, Ninja, Python, and POSIX shell tools. Do not add a
dependency on GNU Make, GCC, glibc, Bash, or GNU utilities. Commands supplied
by users' Makefiles and external campaign infrastructure have separate boundaries.

Follow `.clang-format` (Chromium style) for C++. Test both direct execution and
Ninja conversion when changing shared semantics. Cover recursive builds,
exports, regeneration, or parallel execution when those paths are affected.
Run conversion examples in disposable directories, never the repository root.

## Run the portable checks

From the repository root, build and run the supported test image:

```sh
docker build -t openmktr-test .
docker run --rm openmktr-test
```

[Dockerfile](Dockerfile) builds Openmktr in the Chimera toolchain image and
runs the standard suites without installing GNU utilities or explicitly
requesting Chimera Utils. Core checks have also passed in Alpine's BusyBox
environment.
[The CI workflow](.github/workflows/cpp-ci.yml) uses the same image, checks
changed C++ formatting, and runs a separate sanitizer build. Docker and the
GitHub Actions host are external infrastructure; project checks run inside Chimera.

To run the standard checks directly in Chimera or an equivalent environment
with the listed build requirements:

```sh
ninja -f build.ninja -j4 omktr tests
out/find_test && out/ninja_test && out/strutil_test && out/load_limit_test
python tests/version_generator.py
python tests/correctness.py
python tests/make_compat_regressions.py
python tests/portage.py
python tests/load_limits.py
python tests/portage_validation.py
python tests/cross_version_cache.py
python tests/regression.py
sh testcase/dump/run.sh
```

| Check | Coverage |
| --- | --- |
| C++ unit binaries | Find emulation, Ninja helpers, string utilities, and load admission. |
| `version_generator.py` | Build revision generation, including archives without Git. |
| `correctness.py` | Incremental builds, failures, exports, regeneration, and job limits. |
| `make_compat_regressions.py` | Dependency, rule, variable, restart, and OpenWrt image-refresh regressions. |
| `portage.py` and `load_limits.py` | Portable adapter behavior, load scheduling, recursive overrides, cancellation, and FIFO token return. |
| `portage_validation.py` | Evidence isolation, failure status, and timeout accounting without requiring Portage. |
| `cross_version_cache.py` | Cross-version cache evidence comparisons, seed isolation, and benchmark accounting without requiring ccache. |
| `regression.py` | Checked-in direct/Ninja snapshots and converted POSIX shell fixtures. |
| `testcase/dump/run.sh` | Include-dump smoke test. |

These suites use self-contained expectations; they do not require a GNU Make
reference executable. Some fixtures intentionally return a nonzero status.
The snapshot harness separately tracks existing crashes in
[known_crashes.json](tests/known_crashes.json).

The GNU-free `python tests/incremental_campaign.py` checks the differential
campaign's failure detection. CI and release qualification also run a separate
[external incremental comparison](validation/incremental/README.md) with GNU Make
as the oracle. Its comparison image is separate from the portable image. Missed
rebuilds and stale artifacts block release; unnecessary work is a performance
defect. The existing correctness and snapshot suites remain required.

If Docker, Chimera, or a required tool is unavailable, report the checks you
actually ran and the limitation. Inspect CI through the GitHub integration when
available.
Documentation-only changes need a diff and accuracy review; they do not need
a full runtime test run.

## Add a regression fixture

Add a case in [testcase/](testcase/) or extend a focused Python suite so it
fails before the fix and checks the corrected behavior. In Chimera, select
matching snapshot scenarios with:

```sh
python tests/regression.py --case automake_stdin_recursive
```

To update snapshots after an intentional behavior change:

```sh
python tests/regression.py --record
git diff -- tests/golden.json tests/known_crashes.json
```

Review every changed expectation. Do not record a bug as expected behavior or
add a new crash to the quarantine list to pass tests. Remove recovered cases
from that list; the recorder rejects new crashes and retained recovered cases.

Check changed C++ files inside Chimera, supplying the actual changed paths:

```sh
CLANG_FORMAT_FILES='src/parser.cc src/expr.cc' sh clang-format-check
```

## Check memory safety

For memory safety or undefined behavior changes, run the same sanitizer checks
as CI from the repository root inside Chimera:

```sh
python tools/gen_sanitizer_build.py
ninja -f build.sanitizer.ninja -j4 omktr-sanitized tests
ASAN_OPTIONS=detect_leaks=0 out/sanitized/find_test
ASAN_OPTIONS=detect_leaks=0 out/sanitized/ninja_test
ASAN_OPTIONS=detect_leaks=0 out/sanitized/strutil_test
ASAN_OPTIONS=detect_leaks=0 out/sanitized/load_limit_test
ASAN_OPTIONS=detect_leaks=0 OMKTR_BINARY="$(pwd)/omktr-sanitized" python tests/portage.py
ASAN_OPTIONS=detect_leaks=0 OMKTR_BINARY="$(pwd)/omktr-sanitized" python tests/load_limits.py
ASAN_OPTIONS=detect_leaks=0 OMKTR_BINARY="$(pwd)/omktr-sanitized" python tests/correctness.py
ASAN_OPTIONS=detect_leaks=0 OMKTR_BINARY="$(pwd)/omktr-sanitized" python tests/make_compat_regressions.py
```

The generator creates an isolated ASan/UBSan graph and output directory.
The normal build remains available for comparison.

## Measure performance

Build baseline and candidate binaries with the same toolchain. On Linux, run:

```sh
python tests/bench.py ./omktr
python tests/bench.py ./omktr --baseline /path/to/baseline/omktr
```

Replace `/path/to/baseline/omktr` with the baseline executable. The benchmark
measures conversion, no-op regeneration, flat and nested parallel execution,
incremental no-op builds, peak memory, and binary size.

It also reports paired phase samples and complete wall times under `end_to_end`
for a synthetic 32-output copy workload: configuration plus generation and
execution for a clean build; regeneration checking plus execution for no-op,
single-source-change, and included-configuration-change builds. Incremental
setup and controlled edits are outside the timer. Output contents and unchanged
mtimes are checked after timing. Each workload discards two warmups and retains
all measured samples; the total median is computed from per-run totals, not by
adding phase medians. These small synthetic workloads measure build overhead,
not compiler throughput or full third-party project performance.

The manually dispatched [optional compatibility workflow](.github/workflows/compatibility.yml)
publishes these reports and tests Alpine/musl, Gentoo/glibc, Ninja >= 1.13 FIFO
jobserver interaction, and serial/parallel external GNU Make comparisons.
See the [lane guide](validation/compatibility/README.md) for scope and local commands.

For the child-graph reuse experiment, run
`python tests/recursive_bench.py ./omktr`. It compares direct recursion with
native cached children, including the available CPU affinity by default.
Use `--cache-kind ninja` to measure explicit `build` children instead.
The [investigation guide](docs/recursive-cache.md) documents the cache
safety boundaries, measurement limits and recorded results.

For opt-in compiler-cache experiments across fresh Portage version directories,
see [cross-version reuse](docs/cross-version-reuse.md). That external campaign
is separate from the recursive no-op cache and the portable PR gate.

[PR #1](https://github.com/RihardsPaps/Openmktr/pull/1)
records the original C++ comparison, test results, and GNU dependency audit.
On its fixed workloads, the refactor showed no material runtime or peak-memory
regression and reduced the binary from 894,896 to 836,960 bytes. Those historical
measurements are not a performance guarantee for other builds.

## Find the implementation

Both modes share parsing, evaluation, and dependency resolution.

| Stage | Main source files |
| --- | --- |
| Parse statements and expressions | [parser.cc](src/parser.cc), [expr.cc](src/expr.cc) |
| Cache parsed includes | [file_cache.cc](src/file_cache.cc) |
| Evaluate variables, directives, functions, and rules | [eval.cc](src/eval.cc), [stmt.cc](src/stmt.cc), [func.cc](src/func.cc) |
| Resolve rules and dependencies | [dep.cc](src/dep.cc) |
| Expand recipes and schedule direct builds | [command.cc](src/command.cc), [exec.cc](src/exec.cc) |
| Generate Ninja files and check regeneration | [ninja.cc](src/ninja.cc), [regen.cc](src/regen.cc) |

## Use external compatibility evidence

Read [the campaign guide](validation/README.md) and
[its results matrix](validation/PROGRESS.md) before running `validation/`
scripts or claiming compatibility. The tools target a recorded WSL builder
and pinned third-party releases. They do not replace CI. Never patch release
source trees to make campaign checks pass.
