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

## Follow-up on merged main

Fresh checks of `c2ff318561675caa4c82d7916dcc81254d47972e` in the pinned
Chimera root filesystem also passed. Kati was rebuilt with Clang, musl,
libc++, and Ninja. Separate extractions of the unmodified tar 1.35 release
archive were used for in-source and out-of-source builds, with dependency
tracking enabled and `MAKE` set to the rebuilt Kati during configuration.
Inherited Make flags and Kati jobserver state were cleared for these checks.

Archive: `tar-1.35.tar.gz`

SHA256: `14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e`

| Check | In-source | Out-of-source |
| --- | --- | --- |
| Configure with Clang and Kati dependency bootstrap | Pass | Pass |
| Issue's `--ninja --regen` conversion command | Pass | Pass |
| First `sh ninja.sh -j12 all` | Pass | Pass |
| Repeat `sh ninja.sh -j12 all` | Pass | Pass |
| Delete `src/tar`, then relink at `-j1` | Pass | Pass |
| Touch extracted `src/tar.c`, then rebuild at `-j12` | Pass | Pass |
| Rerun the conversion with `--regen` | Pass | Pass |
| Remove `po/Makefile`, then build | Expected failure | Expected failure |
| Restore `po/Makefile`, then build | Pass | Pass |

Both produced executables reported `tar (GNU tar) 1.35`. The original reported
failure remains unreproduced with a completely configured source tree.

Removing `po/Makefile` reproduced the same top-level failure shape in both
layouts. Ninja printed `FAILED: [code=1] all` and the recursive Kati command;
the child output identified the actual failure:

```text
Making all in po
*** No targets specified and no makefile found.
*** [all-recursive] Error 1
```

This is a controlled diagnostic example, not proof that the reporter's tree
had a missing Makefile. It shows why the top-level Ninja failure alone does
not establish a graph generation defect. Arbitrary recursive shell loops
continue to run as recipes; flattening them would require a separate design
and cannot be inferred from this failure message.

The repeatable check is now saved in `validation/reproduce_issue20.py`:

```sh
python3 validation/reproduce_issue20.py /path/to/tar-1.35.tar.gz /path/to/ckati
```

Run it in Linux with Python 3.12 or newer, Clang, Ninja, and tar's configure
prerequisites available. It creates disposable source/build trees and retains
every command's combined stdout/stderr, exit status, Kati revision, and archive
hash. It does not download sources or modify existing release trees. The
controlled missing-file check restores the file before testing recovery.

Local follow-up validation also passed all three C++ unit binaries, the
Automake stdin/recursive fixture, 106 correctness tests, and six Make
compatibility tests. The compatibility suite initially lacked two auxiliary
validation modules in the copied test tree; after copying them, all six passed.
Docker was unavailable locally; these checks used the existing pinned Chimera
chroot. The PR's portable Docker and sanitizer checks remain the CI gate.

To diagnose the reporter's failure, obtain the failing binary's `ckati --version`
output, the full configure output and exit status, and the complete build log:

```sh
ckati --version
sh ninja.sh -j12 all > tar-build.log 2>&1
```

Also establish whether `po/Makefile` exists and whether `MAKE`, `MAKEFLAGS`, or
`MAKEOVERRIDES` was overridden. No new runtime fix or issue closure is justified
by the current evidence; PR #21 already contains the confirmed stdin fixes.
