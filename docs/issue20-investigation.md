# GNU tar 1.35 investigation (issue #20)

Issue: https://github.com/RihardsPaps/UNIVERSAL_GNUMAKE_TO_NINJA_TOOL/issues/20

The reported `ckati all-recursive` edge is expected with the current conversion
model: recursive Make commands remain opaque Ninja recipes. Their child Kati
processes parse and build the subdirectory Makefiles at execution time. This
change does not flatten arbitrary shell loops into a single Ninja graph.

## Reproduction and finding

On baseline revision `d25e7be`, a clean GNU tar 1.35 source tree configured with
Clang on Ubuntu 24.04 completed conversion and `sh ninja.sh -j12 all` successfully
using the issue's commands. The original runtime failure therefore has not been
reproduced on that revision. Its full child-process error output and executable
revision would be needed to establish the original cause.

A separate Automake compatibility defect was reproduced when configuring tar
in Chimera Linux with `MAKE=/issue20-tool/ckati`. Automake's dependency bootstrap
pipes filtered Makefiles into `ckati -f - am--depfiles`. Kati previously opened a
literal file named `-`, leaving the requested target undefined. Configuration
failed before generating `po/Makefile`, and attempting to build that incomplete
tree subsequently failed in `all-recursive` at the `po` subdirectory.

## Fix

The branch was subsequently rebased onto `6af2912`, which includes PR #19's
initial stdin support. The remaining fixes distinguish stdin from a literal
dash filename and prevent stale Ninja reuse for changed stdin. The restart
copy is now made only when reparsing is required, rather than retaining an
inherited descriptor and environment transport variables on every stdin read.

Treat `-f -` as standard input and read through EOF, independent of pipe size.
Cache the stream separately from ordinary files, so `include -` still opens a
literal pathname. Preserve the input across process restarts after generating
included Makefiles. Always regenerate Ninja when a Makefile comes from stdin;
a timestamp cannot establish whether a new stream matches an earlier one.

`testcase/automake_stdin_recursive.sh` covers the dependency bootstrap, a literal
dash file, direct and Ninja recursive builds, repeated builds, generated-include
restarts, large piped and redirected input, and changed stdin under `--regen`.

## Validation

Built with Clang, musl, libc++, and Ninja in the available Chimera root filesystem.
Before rebasing, all three C++ unit binaries, all 26 correctness tests, and the
include-dump test passed. The snapshot recording added only the new successful
scenario to `tests/golden.json`; the suite matched all 823 scenarios, with 33
existing crash cases quarantined. One earlier run differed only in the output
order of the existing `multiple_output_patterns.mk` case; the repeat passed
unchanged.

After rebasing onto `6af2912`, all three C++ unit binaries, all 106 correctness
tests, all 856 regression snapshots (no quarantined crashes), and the include-dump
test passed. Tar's conversion and two launcher runs also passed with the rebased
binary.

GNU tar 1.35 configured from a clean source tree with dependency tracking enabled
and Kati as its Make executable, then built successfully through the generated launcher
at `-j12`. A second launcher invocation succeeded, and `src/tar --version`
reported GNU tar 1.35. Commands inside the configured source tree:

```sh
FORCE_UNSAFE_CONFIGURE=1 MAKE=/issue20-tool/ckati CC=clang CXX=clang++ ./configure
/issue20-tool/ckati --ninja --regen CC=clang HOSTCC=clang HOSTCXX=clang++ -f Makefile all
sh ninja.sh -j12 all
sh ninja.sh -j12 all
./src/tar --version
```

`FORCE_UNSAFE_CONFIGURE` was needed because the test root filesystem runs as root.
The external tar sources and build recipes are outside Kati's GNU-free boundary.
