# LibreOffice build results and observed Openmktr issues

**Latest follow-up:** [runtime recipe validation](#runtime-recipe-validation)
resolves the nested generated-file reads. Original benchmark results remain
historical.

On 29 October 2026, GNU Make completed a clean LibreOffice 25.2.7.2 build in
828.23 seconds on the user's PC. Openmktr's Aggressive Ninja path crashed during
graph generation in two fresh trees. A direct Aggressive attempt progressed but
used very little of the requested parallelism; it was stopped at the user's
request. **There is no completed Openmktr build time or valid build speed ratio.**

All workloads ran locally in Ubuntu 24.04 under WSL2. The NAS was not accessed.
No Openmktr implementation files or third-party release sources were patched.
The historical LibreOffice compatibility result in `validation/PROGRESS.md`
remains evidence for its original binary, configuration and direct build, rather
than qualification of this Ninja invocation.

The [structured results](../validation/libreoffice-benchmark-results.json) retain
commands, identities, measurements, cancellation status and artifact hashes.
The [evidence archive](../validation/evidence/libreoffice-benchmark-2026-10-29.zip)
contains 49 logs, reports, resource traces, snapshots and scripts, plus an entry
checksum manifest. Names used below refer to files inside that archive.

## Build configuration and timing

| Setting | Value |
| --- | --- |
| Host CPU | Intel Core i9-14900KF; 32 logical processors exposed to WSL |
| Windows physical RAM | 34,130,411,520 bytes, approximately 31.8 GiB |
| WSL resources during workloads | 28 GiB RAM limit, 16 GiB swap, 32 processors |
| Build concurrency | `-j32`; LibreOffice configured with `--with-parallelism=32` |
| Storage | Native WSL Linux filesystem; approximately 690 GiB initially available |
| LibreOffice | 25.2.7.2, from the retained release archive |
| Application compiler | Ubuntu Clang/Clang++ 18.1.3 for both tools |
| Application compiler settings | Release configuration, `-O2 -mtune=generic`; no added LTO/PCH setting |
| GNU comparison | GNU Make 4.3, rather than a switch to GCC |
| Ninja | 1.13.2, through the retained musl launcher |
| Openmktr source revision | `38ddf6d0e4c8bee9e5449992ed1c412205d6e56c` |
| Openmktr build | Chimera Clang 22.1.8, C++17, O2/ThinLTO, musl and LLVM libc++ |
| Compiler caches | Disabled with configure and `CCACHE_DISABLE=1` |
| Filesystem caches | Uncontrolled; not flushed between attempts |

Each main attempt used a fresh extraction and out-of-source build directory.
The selected desktop configuration uses system libraries with the same bundled
library exceptions as the historical campaign. It excludes Java, Skia, help and
bundled spelling dictionaries. This was a resource-intensive clean build of that
configuration, rather than an all-features or highest-compiler-optimization build.
Every configure option is retained in the structured results and harness.

After replacing each lane's directory prefix with a common placeholder, the GNU,
Ninja Aggressive and direct Aggressive `config_host.mk` contents are identical.
`normalized-config-diff.txt` is empty; the structured report records the matching
normalized hashes. Both tools received the configured `ALL_LANGS` value.

The timer covers the complete main build invocation. For Ninja mode, this includes
graph generation. Configuration, bootstrap/fetch/GDB-printer preparation, repeat
build, smoke check and source verification were measured separately. Resource
traces sample WSL memory availability and swap use every two seconds. CPU usage
is accumulated child user plus system time divided by wall time. Peak RSS is the
largest individual process reported by `wait4`, not the simultaneous sum of a
process tree. WSL memory availability is a system-wide measure.

| Attempt | Wall time | Mean CPU cores | Peak individual RSS | Outcome |
| --- | ---: | ---: | ---: | --- |
| GNU Make main build | 828.23 s | 30.66 | 0.727 GiB | Completed |
| Aggressive Ninja, default 8 MiB stack | 76.52 s | 0.90 | 6.586 GiB | Generation exited 139 |
| Aggressive Ninja, unlimited stack | 104.08 s | 0.93 | 6.599 GiB | Generation exited 139 |
| Direct Aggressive, unlimited stack | 445.71 s | 1.27 | 6.530 GiB | Intentionally cancelled |

All four attempts recorded zero swap use. The direct duration includes a brief
debugger attachment immediately before cancellation. Failed-generation durations
and the interrupted direct duration are diagnostic observations, not completed
build timings. Their different lengths do not establish a stack-setting speed
effect. Only one successful GNU clean build was measured; there are no paired
samples, confidence interval or extrapolated Openmktr completion estimate.

## Ninja graph generation crashes

Both fresh Aggressive Ninja attempts exited 139 and printed:

```text
*kati*: Segmentation fault, last evaluated line was .../solenv/gbuild/LinkTarget.mk:961
```

The same crash occurred with an unlimited stack after the initial default-stack
failure. Both attempts had roughly 19.8 GiB of WSL memory still available at the
minimum sampled point and used no swap. Increasing the stack did not provide a
workaround, and the resource traces do not show RAM exhaustion.

The direct generation replay under GDB stopped at SIGSEGV with this call chain:

```text
Evaluator::LookupVar(Symbol)
Evaluator::LookupVarForEval(Symbol)
SymRef::Eval(...)
CommandEvaluator::Eval(...)
NinjaGenerator::PopulateNinjaNode(...)
NinjaGenerator::Generate(...)
Run(...)
main
```

`aggressive-gdb-generation.log` preserves the full stack. The last reported
Makefile location is the header-target recipe containing
`$(eval $(gb_LinkTarget__get_headers_check))`. That macro dynamically assigns a
target-specific `COMMAND`, followed by its expansion in the recipe.

The stack establishes a variable-lookup failure during recipe expansion for
Ninja emission. It does not establish the invalid variable, allocation lifetime,
exact source instruction or ownership error. Dynamic target-specific assignments
and retained recipe scopes are a useful investigation lead, not a verified root
cause. No sanitizer replay, reduced reproducer or Safe/off control was run, so
these observations do not show that Aggressive optimizations uniquely cause the
crash.

The graph never reached Ninja recipe execution. Consequently there is no
Aggressive Ninja LibreOffice executable, runtime smoke pass or repeat-build
result from this investigation. Raw evidence is in
`initial-failed-aggressive-build.log`, `aggressive-build.log` and their resource
traces.

## Generation probes outputs that do not exist yet

Each failed fresh generation logged 208 `No such file or directory` messages,
including attempted reads of generated component descriptions and installer
file lists:

```text
.../workdir/ComponentTarget/comphelper/util/comphelp.component.filtered
.../workdir/InstallModuleTarget/scp2/writer.filelist
.../workdir/InstallModuleTarget/scp2/impress.filelist
```

The successful GNU main-build log contains no such messages. Openmktr encounters
these reads while expanding the clean graph before the producing recipes have
run. Their relationship to the later SIGSEGV is unproven. They are a separate
compatibility warning: merely preventing the crash would not establish that
the emitted commands contain the correct generated-file contents or inputs.

Generation also performed real helper compilation. Both failed trees retain
`workdir/CObject/solenv/bin/concat-deps.o`, and their logs contain compilation and
link messages for `concat-deps`. Neither log contains a `[build CXX]` message.
Therefore “before any compilation” is too broad: the failure occurred before
Ninja execution, after generated-include/helper work had already compiled a C
object. Any future timing split must account for this work rather than moving
it outside the build timer.

## Direct execution underuses the job budget

The fresh direct attempt explicitly used
`--graph_optimizations=aggressive -j32` against `Makefile.gbuild`. It created
32 worker threads plus the main thread, yet repeated process samples usually
showed only one or two compiler processes. It averaged 1.273 CPU cores over the
partial run, approximately 4.0% of the 32 exposed logical CPUs. GNU Make averaged
30.655 cores, approximately 95.8%, across its completed build.

The partial direct run logged 781 C++ compilation starts and retained 784 object
files, including other C/helper objects. Those counts do not mean 781 C++ files
completed successfully: the final compiler invocation was interrupted. The GNU
build logged 9,341 C++ starts and retained 9,507 object files. The differently
completed workloads cannot support a compiler-throughput ratio or total-time
speedup claim.

The direct process held approximately 6.5 GiB RSS while substantial WSL RAM
remained available. Its minimum sampled available memory was approximately
18.7 GiB; it used no swap. Low concurrency therefore coincided with spare CPU
and memory capacity.

Two wait-channel samples help bound the observation. An earlier live sample
showed 30 futex waits and three poll waits across 33 threads. Immediately before
stopping, 32 threads were in `futex_do_wait` and one in a poll wait.
`direct-stop-process-snapshot.json` preserves the final snapshot. Host GDB could
not unwind the musl worker stacks into meaningful scheduler frames; most frames
in `direct-worker-stacks.log` are unknown. Futex waits alone cannot identify a
particular dependency, lock, worker-pool defect or deadlock. The build was still
advancing, so no permanent deadlock is claimed.

The worker queue, dependency ownership/ranks and interaction with header
prerequisites are investigation targets. Their causal role remains unresolved.
The direct attempt does not exercise Ninja emission or establish that Ninja's
Aggressive graph-import features work for LibreOffice.

## Direct mode drops header directory dependencies as cycles

Direct Aggressive emitted eight `Circular ... dependency dropped` messages;
GNU's successful main-build log emitted none. All eight involve
`workdir/Headers/Library/.dir` or `workdir/Headers/Library/` and these libraries:

- `libsal_textenclo.so`, with both directory forms.
- `libuno_sal.so.3`, with both directory forms.
- `libunoidllo.so`, `libreglo.so`, `libstorelo.so` and
  `libuno_salhelpergcc3.so.3`, with `.dir`.

The structured report retains every exact edge. These are observable differences
in dependency handling. The current evidence does not distinguish genuine
source cycles from synthetic directory edges or scheduling/ownership artifacts.
It also does not establish whether the dropped edges cause the low parallelism
or would cause an incorrect finished artifact. Both ordering correctness and
parallel scheduling require focused follow-up.

## The top level wrapper also showed low concurrency

The first diagnostic used the ordinary top-level command:

```sh
omktr build --graph_optimizations=aggressive -j32 MAKE=/absolute/path/to/omktr build
```

Its outer Ninja graph had four actions. The main action invoked Openmktr's
direct executor recursively for LibreOffice's `Makefile.gbuild`. Process
inspection showed `KATI_JOBS=32`, a private jobserver FIFO, and inherited
`KATI_GRAPH_SETTINGS=v1:aggressive`; the child command requested `-j 32`.
Only one or two compiler jobs were observed across multiple samples.

That initial attempt was stopped as an exploratory diagnostic. Its completed
wall/CPU measurement was not retained, and its initial resource-trace filename
was reused by the next attempt. Do not reconstruct a numerical result from the
short outer Ninja log. `diagnostic-top-level-report.json` and
`diagnostic-top-level-aggressive-build.log` preserve the available evidence.

The later direct invocation reproduced underutilization without that outer Ninja
wrapper. This weakens an explanation confined solely to the outer wrapper or
its FIFO, but does not isolate the scheduler's root cause.

## GNU build validation and nonfatal diagnostics

GNU configuration took 9.54 seconds and bootstrap preparation took 0.05 seconds.
Configuration plus preparation plus main build totals 837.81 seconds. These
phase sums exclude extraction, measurement bookkeeping and final verification;
they are not a separately measured end-to-end stopwatch total.

The successful GNU build was followed by:

- A successful repeat invocation in 5.46 seconds, with no compilation output.
- A successful headless text-to-PDF conversion in 0.61 seconds; the output is
  nonempty and starts with `%PDF-`.
- A version check reporting `LibreOfficeDev 25.2.7.2`.
- Original-source verification covering 147,364 files/symlinks, with zero
  differences. Both failed Ninja trees and the stopped direct tree also passed
  that original-source check.

Hashes and sizes for `soffice.bin` and the Writer, Calc, Impress and VCL shared
libraries are retained. Smoke validation covers application startup and Writer
PDF conversion, rather than the full LibreOffice test suite or all GUI features.

Both tools also logged `fatal: not a git repository` from metadata probes of
release trees without `.git`. The successful GNU main-build log contains four
such messages, showing that their wording alone does not identify the build
failure. GNU logged 15 compiler/parser warnings, including deprecated Bison
syntax, grammar conflicts, generated-code warnings and gpgme++ copy-constructor
warnings. The partial direct run logged seven warnings. Different completion
points make those warning totals unsuitable for a tool-quality comparison.

## Setup and evidence limitations

The initial attempt to link Openmktr statically failed because the existing
Chimera environment lacked static C/C++ runtime archives, including libc++,
libc++abi, libunwind, atomic and libc. A dynamic musl build succeeded. Executing
the first dynamic binary directly in Ubuntu failed because its default musl
interpreter path was absent. The disposable build graph was then linked with
an absolute private musl interpreter and RUNPATH. The final tested binary is:

```text
SHA256 4b253967d49bd488a4d8cd31e79a243530b656d0e93819fd4e01277456ae9dad
/root/openmktr-libreoffice-fullblast-20261029/omktr
```

This is a local dynamically linked build of the recorded source revision, not
an official static release binary. Its source snapshot, disposable graph and
private runtime remain on the PC; runtime hashes are in the structured report.
The archive includes `tool-build.log`. These setup failures were resolved before
the measured application builds and are separate from LibreOffice failures.

The first GDB replay targeted `omktr build`. GDB stayed with the supervisor and
detached from the generation child, so that log ends with `No stack`. A second
replay targeted `--ninja --regen` directly and captured the relevant SIGSEGV
stack. Neither debugger replay is a benchmark sample. No Safe/off control was
run, and no source-level memory-safety mechanism is claimed established.

The user requested detailed issue documentation rather than completion of the
Aggressive benchmark. After the final worker snapshot, SIGTERM was sent to its
process group. The interrupted compiler reported Error 143 and Openmktr returned
1. That final status is classified as user cancellation, not a spontaneous build
failure. No Openmktr, Clang++ or GDB processes remained after stopping and source
verification.

The prior `.wslconfig` settings were restored: 24 GiB RAM, 16 GiB swap and 32
processors. The restored limit applies at the next WSL restart; no restart was
forced during finalization. Build trees and logs remain available for debugging.
No implementation fix, third-party source patch or compatibility promotion was
made.

## Reproduction and follow up

The retained Linux workspace is
`/root/openmktr-libreoffice-fullblast-20261029`. The source archive is
`/root/universal-tool-campaign-20260927/sources/libreoffice-25.2.7.2.tar.xz`, with
SHA-256 `21b38b7f429d305b229fb70f3294dd6ccd803f52dad0463d0923cdf143e9c8bd`.
It was an already retained input, not a newly qualified latest release.

`run_benchmark.py` in the evidence archive records the exact environment,
configure options, full build commands and accounting. It is a fixed-path
campaign harness: existing lane directories intentionally prevent an accidental
clean-run overwrite. Adapt it to a new disposable workspace before another
campaign; do not remove the retained trees just to satisfy that precondition.

For a focused generation reproduction in an existing prepared tree, export its
`SRCDIR`, `BUILDDIR` and `WORKDIR`, use the recorded `MAKE` and `ALL_LANGS`
assignments, and replace the first `build` supervisor argument with
`--ninja --regen --ninja_dir=.openmktr`. The main Makefile argument remains
`-rs -f "$SRCDIR/Makefile.gbuild"`, with target `build` and `-j32`.
The exact commands are retained in `report.json` and the generation debugger log.

The next implementation investigation should reduce the variable-lookup crash
around recipe-time target-specific assignment, validate generated-file reads
against a clean GNU build, and isolate the direct executor's cycle handling and
worker underutilization with a small shared-prerequisite graph. A successful
full build, repeat build, runtime smoke and source-integrity checks would be
required before another performance comparison. Profile controls and sanitizer
evidence would be required to attribute a failure specifically to Aggressive
optimizations or to a particular memory-lifetime mechanism.

## Implementation follow up on 29 October

The working-tree candidate fixes the variable lifetime crash, loss of parallel
progress in graphs containing cycles, synthetic parent-directory cycles, and
malformed response-file commands. These changes have local regression and
sanitizer coverage. The broader clean LibreOffice Ninja graph still performs
premature nested generated-file reads; that part of the original plan remains
unresolved. Successful graph emission alone does not qualify the whole build.

The [fix results](../validation/libreoffice-fix-results.json) and
[fix evidence archive](../validation/evidence/libreoffice-fixes-2026-10-29.zip)
contain the patch identity, baseline failures, candidate validation, bounded
commands and remaining limits. The original benchmark archive above is preserved.

### Confirmed causes and changes

ASan on the reduced baseline reproducer reports a heap-use-after-free in
`Vars::Lookup`, reached through `Evaluator::LookupVar`. A recipe-time
target-specific assignment deletes the old variable in `Vars::Assign`, while
the dependency node's copied scope still references it. The reproducer fails in
Off, Safe and Aggressive profiles, in direct and Ninja execution. This is not
an Aggressive-only bug.

Replaced recipe bindings now move into evaluator-owned `unique_ptr` storage and
are released when the evaluator is destroyed. Active scopes see assignments to
their own target immediately, and later target recipes refresh bindings changed
by earlier recipes. Ninja expands prerequisite recipes before dependent recipes
while preserving emitted-node order and rule numbering. The fix neither ignores
the crash nor permanently leaks the replaced values.

The scheduler previously discarded every dependency rank after encountering any
cycle. Unknown ranks disabled cooperative work, including independent leaves
outside the cycle. A counter-based baseline fixture reaches only one active
worker with eight requested jobs. The candidate ranks strongly connected
components and their condensed acyclic graph, retaining strict rank descent
between components. The same fixture reaches eight active workers and checks
that the budget is not exceeded, including recursive execution. Existing
ownership and cross-thread cycle checks remain in place.

Direct dependency construction also synthesized parent-directory edges even
when the directory's graph depended back on the file being built. The candidate
checks reachability before adding this optional edge. It preserves explicit
Makefile dependencies and ordinary generic directory rules. The reduced baseline
fixture emits a circular-dependency warning; the candidate does not.

The bounded Ninja SAL link exposed a further error: recipe-time `$(file >...)`
returned a shell `printf` command as its Make value. LibreOffice embeds that
function inside response-file-name construction, producing malformed commands
such as `RESPONSEFILE=printf ... /tmp/name/tmp/name`. File writes now queue their
execution-time effect and return the required empty Make value. The baseline
response-file fixture fails; the candidate produces the correct file contents.

### Local validation and its boundary

The candidate passes the normal gate in the local pinned Chimera chroot: C++
units, version checks, 180 correctness tests, compatibility regressions,
858/858 snapshot scenarios and the include-dump smoke. It also passes the CI
ASan/UBSan Python suites for Portage, load limits, correctness, graph
optimizations, workspaces, Ninja jobservers and compatibility, plus Aggressive
correctness and the sanitized C++ units. Changed C++ files are formatted with
Chimera's clang-format. The optional Portage check requiring a binary configured
with an absolute Ninja path was skipped; this candidate uses PATH-based Ninja
selection. Docker Desktop's Linux-engine pipe was unavailable, so
the Docker image commands were not run; these results are local chroot checks.

The real LibreOffice graph replay completes without SIGSEGV. Its broader graph
still logs premature generated-file reads. The SAL-only graph generates without
those missing-output probes and supports a bounded link of
`instdir/program/libuno_sal.so.3`. A direct Aggressive SAL build reaches 32
simultaneous compiler processes and emits no cycle warnings. Eight selected
objects match GNU Make byte-for-byte across direct and Ninja modes.

Raw object goals under GNU Make lack include/define settings normally inherited
from the library target. Validation therefore enters the SAL library scope in
each mode and forces the same eight selected objects to rebuild. It also checks
the resulting library link and repeated invocations. These are correctness and
concurrency checks, with a 300-second execution cap; they are not paired clean
performance samples or a new full LibreOffice benchmark.

Standalone Ninja can retain LibreOffice's temporary `Misc/PHONY` prerequisites
from before compiler depfiles exist. The supported `omktr build` entry point
regenerates the graph after those files appear. Repeat checks therefore use
that entry point rather than treating the execution-only launcher as a
regeneration interface. Repeat generation may still execute helper/link edges;
compiler activity and selected-object timestamps are recorded separately.

Final repeats in GNU, direct Aggressive and the supported Ninja entry point
launch no compiler processes and leave all eight selected object timestamps
unchanged. The SAL shared library also matches byte-for-byte across the three
modes, with SHA-256
`46057d6f9470dbb44681079a60d55ce1c7193cff4f04a9e06a09697e2e9689ae`.
The bounded Ninja repeat takes about 46 seconds, dominated by graph validation
and generation; this overhead remains visible rather than being represented as
a fast execution-only no-op result.

Original-source verification still reports 147,364 files/symlinks checked and
zero differences. All work remains on the PC. No full LibreOffice build,
benchmark rerun, merge or publication was performed for this follow-up.

### Remaining generated file limitation

The broader graph replay logs 211 missing-file messages involving generated
component descriptions and installer file lists. A nested `$(shell cat ...)`
can feed a Make-language transformation, which needs its value during recipe
expansion. Returning an ordinary shell substitution cannot safely defer the
surrounding Make computation. The response-file fix handles queued writes; it
does not solve this general nested-read problem.

Those diagnostics were not suppressed, and no successful full LibreOffice Ninja
build or general speedup is claimed. Supporting the broader graph requires a
runtime recipe-evaluation mechanism for these nested computations, with focused
producer/consumer tests, before a full benchmark is appropriate.

## Runtime recipe validation

This follow-up supersedes the generated-file limitation above. All work stayed
on the existing branch and ran on the user's PC. No full benchmark was rerun.

### Implementation

Generation inspects structural expressions without executing them. Recipes with
prerequisites that contain shell/file operations, or unresolved computed
references, use runtime Make evaluation after Ninja completes their producers.
The complete recipe is deferred, preserving Make transformations and quoting.
Recipes without prerequisites retain their existing expansion path.

Versioned, checksummed, content-addressed sidecars preserve expression trees,
variable definitions and origin/flavor, target-specific context, automatic
variable inputs, exports, shell settings and recipe flags. Combined pattern and
explicit target-variable appends have a structural representation. Global
contexts are shared across recipes within an assignment epoch; target bindings
remain separate. Unchanged records retain timestamps, and sidecars participate
in graph dependency tracking. Missing, incompatible or corrupt records produce
a regeneration diagnostic. No public CLI was added.

The private evaluator uses the existing supervisor and shared jobserver. It
does not resolve prerequisites. Local `eval` and response-file effects occur at
execution time. Dependency-rule changes, assignments to another target, and
multiple double-colon groups are explicitly rejected on this deferred path.

A synchronized worker test caught an intermediate implementation incorrectly
classifying the evaluator as a recursive build and placing its recipes in a
serial Ninja pool. The final emitter supervises these as ordinary recipes;
two independent consumers now overlap and never exceed `-j2` in every profile.

### Regression and local gate results

The previous candidate was reconstructed from the retained patch on the recorded
base, with normalized line endings in a disposable Chimera directory. Its
focused suite fails 24 Ninja subcases. The candidate passes eight focused tests
covering 45 profile/mode cases: quoted reads, installer transformations,
computed calls, `foreach`, local effects, automatic variables, target-specific
origin/flavor, `.ONESHELL`, exports, stable fixture repeats, corrupt sidecars
and synchronized concurrency.

The local Chimera normal gate passes: C++ units, version checks, 180 correctness
tests, eight focused tests, 53 graph tests, 30 workspace tests, seven jobserver
tests, six compatibility tests, 858 snapshots and the include-dump smoke test.
Formatting and `git diff --check` pass. The focused suite is added to Docker and
sanitizer CI commands.

ASan/UBSan units and CI Python suites pass, including Aggressive correctness.
One optional Portage test remains skipped because this build selects Ninja via
PATH. One sanitizer workspace run failed because GC did not create its expected
compressed archive; both its focused retry and a complete 30-test retry passed.
The failure is retained and its cause is unestablished. Docker Desktop's Linux
engine is unavailable; the Docker image gate was not run.

### Bounded LibreOffice results

The final broad graph generates in 38.49 seconds without SIGSEGV or the earlier
missing generated-file probes. A selected graph builds both
`comphelper/util/comphelp.component` and `setup_osl.ins` with real prerequisites
at `-j32`. After explicitly removing those final outputs, generation takes
50.37 seconds, execution 1.72 seconds and repeat execution 1.67 seconds.
Each attempt has a 300-second process-group cap. These are validation timings,
not comparable clean-build speed measurements.

The component output is byte-identical across Ninja, direct Openmktr and GNU Make
(`a08a119dc8089b28207be8fb99413c38dbe38638a9ed23c24297d25990ff6309`).
Installer files differ only in identifier-list ordering from Perl hash iteration.
Sorting those simple identifier lists makes the complete outputs identical
(`25b1c0cdaae66896b380f1fa20d57e59fcb1eed1112b00e256209a9ecf0e9e67`).
Raw files are retained. The selected SAL library also matches across all three
configurations. Source verification checks 147,364 files/symlinks with no changes.

Two early graph attempts hit their caps while contexts were copied per recipe or
definitions repeatedly scanned. Shared global contexts and early termination of
conservative inspection removed that bottleneck. One retry recorded a `(deleted)`
executable path because the validation driver replaced its running binary;
stable-binary replay passed. These attempts remain in the evidence.

Selected-target repeats can rebuild intermediate files and rewrite randomized
installer lists; direct repeats also changed output timestamps. Optimal no-op
behavior is not established. A pre-existing direct-mode limitation involving
target-specific exports inside shell-function expansion was observed; exports
to recipe processes are covered here.

The [structured results](../validation/libreoffice-nested-recipe-results.json)
and [new evidence archive](../validation/evidence/libreoffice-nested-recipes-2026-10-29.zip)
retain commands, hashes, failures, test logs, raw selected outputs and comparison
details. Earlier evidence remains unchanged. No full-build speed ratio is claimed.
