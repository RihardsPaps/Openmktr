# Validate external Makefile builds

For current stable release inputs and dependency upgrades, use
[`dependency-campaign.json`](dependency-campaign.json) and the
[dependency guide](../docs/dependencies.md). Their pending statuses are separate
from the historical results below. `qualify_dependencies.py` uses the refreshed
distribution LLVM/Ninja toolchain and a durable 200-command ledger on the PC.
The user authorized a total cap of 400 for the Linux/Alpine follow-up after the
earlier extensions to 250 and 335;
`requalify_dependencies.py --command-limit=400` continues the same ledger without
resetting its consumed reservations. Each comparison retains one discarded
warmup pair and seven measured alternating pairs.
`benchmark_graph_budget.py` retains the earlier, exhausted 20-command protocol
for its historical evidence; it is not the current qualification runner.
The [dependency upgrade results](dependency-upgrade-results.json) record the
current qualification, failed promotion gate and remaining platform limits.
The [Linux/Alpine follow-up](dependency-followup-results.json) preserves those
results and records the subsequent workspace fixes and controlled Linux retry.
That retry now completes the Linux 7.3 j8/j16 matrix and direct clean/repeat
checks at 394 of 400 commands. All correctness and source-integrity checks pass;
both clean-build confidence intervals cross zero, so strict performance
qualification remains unresolved and safe stays the default. The separate
Linux 6.12 historical-toolchain variant passes its j8 clean comparison.

Use this directory to investigate real builds and inspect recorded compatibility
evidence. [The results matrix](PROGRESS.md) names the pinned releases,
configurations, and limits. It records 41 selected passes, one user-requested
AOSP skip, and an unresolved NetBSD dialect gap; complete ordered campaign
acceptance has not passed.

## Choose the right checks

For a portable pull-request gate, follow [the contributor
guide](../CONTRIBUTING.md#run-the-portable-checks).
The campaign helpers target Ubuntu 24.04 in WSL and a pinned Chimera chroot.
Many contain absolute paths to `/root/universal-tool-campaign-20260927` and
this checkout under `/mnt/c`. They are debugging and evidence tools for that
builder, not a portable replacement for CI.

The WSL campaign host and third-party build recipes can use GNU tools. The
project's GNU-free build and test guarantee applies to the supported Chimera
path, as described in [the README](../README.md).

## Differential incremental correctness

[`incremental.py`](incremental.py) compares changed target sets, deterministic
output hashes against clean GNU Make builds, compiler dependency coverage, and
unnecessary work in direct and Ninja modes. The [suite guide](incremental/README.md)
describes all nine controlled scenarios, reusable external-project manifests,
retained evidence, and the separate comparison image. Missed rebuilds, invalid
artifacts and incomplete validation block release; excessive rebuilding is
reported separately as a performance defect. The bundled representative build
fixtures do not establish incremental correctness for the third-party releases
in the existing matrix. Qualify their project adapters before making that claim.

## Benchmark clean builds for issue 37

[`benchmark_issue37.py`](benchmark_issue37.py) compares external GNU Make and
Openmktr-generated Ninja execution using unmodified FFmpeg 7.1.1 and Linux 6.12
release archives. It requires the recorded Linux host tools, including Clang 18,
LLVM tools with the `-18` suffix, Ninja, and each project's configure/build
prerequisites. GNU Make is the external comparison tool; this helper is separate
from the GNU-free portable gate.

For example, on the recorded WSL builder with a native Linux output directory:

```sh
python3 validation/benchmark_issue37.py \
  --root /root/openmktr-issue37-benchmark \
  --archives /root/universal-tool-campaign-20260927/sources \
  --omktr /path/to/omktr --project ffmpeg --jobs 8 16 --repeats 2
```

Repeat with `--project linux`. Every run gets a fresh output directory; use a
new `--label` when repeating a campaign. The helper preserves logs, archive and
binary hashes, configuration hashes, artifacts, CPU/wall measurements, and source
content checks. Configuration, graph generation, and artifact verification remain
outside the execution timer. Reports also retain the configuration phase,
`phase_total_wall_seconds` (configuration + generation + execution), and
`end_to_end_wall_seconds` (elapsed configuration through execution, including
the intervening configuration hashes and object scans). Final artifact checks
and smoke tests remain outside both totals. It rejects generation that compiles additional
object files. Compiler caches are disabled; filesystem caches stay warm.

The default order alternates Make/Ninja and Ninja/Make. `--modes ninja` supports
candidate rechecks against retained baselines. Optional `--trace-exec` uses
strace for a separate diagnostic run; traced timings must not be mixed with the
untraced comparison. Kbuild's recursive evaluation remains part of execution,
because its generated outer graph invokes child Openmktr processes at runtime.

For repeatable no-op and single-file-change overhead measurements, use
`tests/bench.py` as described in the [contributor guide](../CONTRIBUTING.md#measure-performance).
Those are synthetic fixtures, not incremental FFmpeg or Linux measurements.
Historical results retain their original execution-only meaning; rerun this
helper to obtain end-to-end totals rather than inferring them from old reports.

For paired Openmktr baseline/candidate comparisons on the same Linux builder,
use `benchmark_ninja.py`. It needs Python 3.12+, Ninja 1.13.2+, distribution Clang, and the
unmodified `ffmpeg-7.1.1.tar.xz` or `linux-6.12.tar.xz` release archive:

```sh
python3 validation/benchmark_ninja.py --root /disposable/report/ffmpeg \
  --archives /path/to/archives --project ffmpeg \
  --baseline /path/to/baseline/omktr --baseline-revision BASE_REVISION \
  --candidate /path/to/candidate/omktr --candidate-revision CANDIDATE_REVISION
```

Repeat with `--project linux` and a new report directory. `--project synthetic`
needs no archive and checks controlled included-file content changes. Defaults
are both `omktr build` and legacy generation/launcher entry points, `-j8` and
`-j16`, two discarded warmup pairs, and seven measured pairs. Baseline/candidate
order alternates; each pair starts with fresh configured build directories.
Build both tools with the same compiler, libraries, optimisation and link flags.
Run performance workloads sequentially without simultaneous test/build jobs.

Each clean build is followed by no-op, touched-source, missing-object and
Makefile timestamp-change invocations. Source timestamps are restored; release
contents are never edited. Configuration and verification are outside the build
timer. Legacy runs record separate generation/regeneration and execution times;
single-command runs record the complete invocation. Reports retain commands,
statuses, binary/archive/configuration hashes, raw samples including warmups,
object changes, graph bytes, and source-content/symlink integrity checks. Release
artifacts must match between tools and incremental scenarios. FFmpeg also runs
the encode smoke check. Candidate no-op object rebuilds and unrelated-object
changes fail validation. Baseline no-op work is retained as performance-defect
evidence, rather than excused in the candidate.

`report.json` includes paired improvements and a deterministic bootstrap interval
for their median. Fewer than seven complete measured pairs cannot support an
improvement/regression flag; incomplete pairs never enter comparisons. Treat
intervals as workload evidence, not universal performance guarantees, and inspect
any apparent regression with a separate repeated run. Failed commands remain in
`commands.jsonl`; a failed run retains its partial report. Existing reports are
never overwritten. Collect execution traces and graph/statistics diagnostics in
separate runs, outside these timing samples.

The [recorded comparison](../docs/ninja-performance.md) and
[individual samples](ninja-performance-results.json) disclose complete runs,
excluded interference, and incomplete Linux runs caused by baseline compiler
crashes. They do not establish the full clean/incremental acceptance objective.
The [PR candidate recheck](pr-performance-results.json) separately records the
submitted source hashes, complete synthetic matrix and FFmpeg build/j16 repeat.
`--llvm-version 19` selects version-suffixed LLVM 19 tools. The isolated
[`ninja/Dockerfile.llvm19`](ninja/Dockerfile.llvm19) supplies external release
prerequisites; use identical baseline/candidate binaries and explicit matching
container stack limits. This image does not replace the Chimera gate.

The retained [issue 37 measurements](issue37-results.json) identify each measured
revision and phase. These selected configurations do not reproduce every detail
of the original reporter's Gentoo/Ryzen environment.

## Preserve evidence

Keep release source trees separate from disposable build trees. Do not patch
third-party sources to obtain a pass. Record the release or source revision,
archive digest where applicable, configuration, Openmktr revision or binary digest,
command, exit status, and artifact verification. Include repeat builds and
original-source integrity checks before marking a project as passed.

[`phase.py`](phase.py) writes command output to `REPORT_DIR/logs/` and appends
a timestamp, working directory, command, exit status, elapsed time, and log
filename to `REPORT_DIR/PROGRESS.md`. Its default report directory is `/report`.
For example, on Linux from the repository root:

```sh
python3 validation/phase.py --name kati-version --cwd "$PWD" \
  --report "$PWD/validation" -- "$PWD/omktr" --version
```

This records a version probe only. It is not a compatibility pass. Full campaign
logs are ignored under `validation/logs/`; the checked-in
[historical ledger](history.log) preserves their recorded names and phase results.

## Check the recorded WSL candidate

Only on the builder with the campaign's paths and chroot prepared, run these
commands from the repository root:

```sh
python3 validation/check_chimera_format.py
bash validation/validate_candidate_chimera.sh
```

The format helper checks changed C++ files with the chroot's clang-format.
The candidate helper copies selected checkout files into the existing Chimera
workspace, builds Openmktr, and runs C++ units, version generation, correctness,
Make compatibility, snapshots, and the dump smoke test. It uses the existing
chroot state; CI supplies the portable clean-image and sanitizer checks.

## Diagnose recursive Ninja failures

A Ninja failure at an `all-recursive` edge identifies the failed recipe, but
its child-process output is needed to identify the cause. Recursive shell loops
remain recipes; child Openmktr processes evaluate their subdirectory Makefiles at
execution time.

For a failing build, save the executable revision and complete launcher output:

```sh
omktr --version
sh ninja.sh -j12 all > tar-build.log 2>&1
```

Check the configure exit status, generated subdirectory Makefiles, and any
overrides to `MAKE`, `MAKEFLAGS`, or `MAKEOVERRIDES`. A missing `po/Makefile`
can produce this child error in GNU tar 1.35:

```text
Making all in po
*** No targets specified and no makefile found.
*** [all-recursive] Error 1
```

That controlled example does not establish what caused the reporter's failure.
The original [issue
#20](https://github.com/RihardsPaps/Openmktr/issues/20)
failure remained unreproduced with a completely configured tree. The confirmed
Automake stdin defects were addressed in PR #21: `-f -` reads standard input,
`include -` still reads a literal dash filename, restarts preserve the input,
and changed stdin cannot reuse a stale Ninja graph. The
[Automake fixture](../testcase/automake_stdin_recursive.sh) covers those paths.

## Reproduce the GNU tar check

On Linux with Python 3.12 or newer, Clang, Ninja, and tar's configure
prerequisites, provide a tar 1.35 release archive and the Openmktr executable:

```sh
python3 validation/reproduce_issue20.py /path/to/tar-1.35.tar.gz /path/to/omktr
```

Replace both paths with existing files. The helper creates disposable in-source
and out-of-source trees, clears inherited Make and Openmktr jobserver state, and
retains combined command output, exit statuses, Openmktr revision, and archive hash.
It checks initial and repeated builds, relinking, changed-source rebuilds,
regeneration, and recovery from a deliberately removed `po/Makefile`. It restores
that file before the recovery check and does not download sources or modify
existing release trees.

The recorded checks at revision `c2ff318561675caa4c82d7916dcc81254d47972e`
passed both layouts in the pinned Chimera chroot. The archive SHA-256 was
`14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e`.
See [the archived investigation](history.log) for historical suite counts,
optimized-Python checks, and the distinction between confirmed fixes and the
unreproduced report. These results do not justify claiming a new runtime fix
or closing the original report without evidence of its cause.
