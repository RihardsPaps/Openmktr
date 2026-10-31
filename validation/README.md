# Validate external Makefile builds

For pinned campaign inputs, use [dependency-campaign.json](dependency-campaign.json)
and the [dependency guide](../docs/dependencies.md). An archive hash or historical
pass does not qualify a new release. The [performance summary](../docs/ninja-performance.md)
records completed comparisons and their limits; the
[evidence index](evidence/README.md) links the preserved reports and archives.

[The results matrix](PROGRESS.md) records 41 selected passes, one user-requested
AOSP skip and NetBSD outside the supported Make dialect scope. It does not
establish compatibility for every version, target or configuration.

## Choose the right checks

For a portable pull-request gate, follow [the contributor
guide](../CONTRIBUTING.md#run-the-portable-checks).
Historical campaign helpers target Ubuntu 24.04 in WSL and a pinned Chimera chroot.
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

Use `--require-no-regression` for an acceptance run against either an Openmktr
revision or external GNU Make. It exits with status 3 unless every requested
timing and peak-process-RSS cell has all samples, at least seven pairs, a median
candidate/baseline ratio at most 1.00 and a bootstrap 95% upper bound at most 1.05.
The upper bound describes uncertainty; it does not permit a median slowdown.
Reports always include `no_regression` and `no_regression_passed`, including
exploratory runs without the flag. Failed or incomplete evidence cannot pass.

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

## Qualify the core overhaul on the PC

Use [`qualify_core.py`](qualify_core.py) with a copy of
[`core-qualification.example.json`](core-qualification.example.json). Replace
all paths and revision labels with the PC's actual tools and configuration.
`root` must be a new disposable directory. The selected release archives must
match [dependency-campaign.json](dependency-campaign.json); obtain them before
running. LibreOffice's configuration JSON must contain the reviewed
`configure_options` list. Its three probe paths must identify an enabled source,
its object and an unrelated object in that configuration. The external build
host needs the projects' configure/build dependencies, Python 3.12+, `pypdf`,
GNU Make, Clang/LLVM and Ninja 1.13.2+. GNU Make is only the comparison tool.

Inspect the concrete matrix without executing builds:

```sh
python3 validation/qualify_core.py --config /path/to/qualification.json --plan-only
```

Run sequentially on the PC with other performance workloads stopped:

```sh
python3 validation/qualify_core.py --config /path/to/qualification.json
```

The driver requires both direct and build modes, two warmups and seven measured
pairs per requested job count. It alternates tool order, verifies clean and
incremental artifacts, checks source integrity, and retains logs, hashes,
application smoke products and every sample. LibreOffice covers source touch,
missing object and graph touch in addition to clean/no-op and Writer/Calc/Impress.
By default, completed LibreOffice source/build trees are removed after retaining
evidence; `--retain-builds` keeps them. Other evidence is never overwritten.
Failure leaves a partial `qualification.json` with `release_qualified: false`.

Timing and memory each require every requested cell, a median candidate/GNU
ratio at most 1.00 and a bootstrap 95% upper ratio at most 1.05. Missing samples
cannot qualify. Memory is `wait4` peak individual-process RSS, including reaped
children, rather than summed simultaneous tree memory. LibreOffice additionally
retains sampled tree RSS. Build timers include graph generation/regeneration;
configuration and verification remain outside them. Failed commands or artifact
checks fail the campaign independently of timing. These PC runs remain unrun
for the architectural overhaul; historical selected passes do not fill the gate.
