# LibreOffice 25.2.7.2 full clean-build comparison

Campaign: `libreoffice-pair-20261029-200435`, run locally on 29 October 2026.
Both tools completed verified clean builds. GNU Make wins this single pair:
Openmktr Aggressive takes **902.45 seconds**, versus **821.39 seconds** for GNU,
an additional **81.06 seconds / 9.87%**. A new wildcard-expansion blocker was
fixed before the successful Openmktr attempt. The repeat build exposes much
larger overhead and unnecessary packaging work.

[Machine-readable results](../validation/libreoffice-full-comparison-results.json)
contain the exact commands, measurements, identities and complete installed-file
manifests. [Full evidence](../validation/evidence/libreoffice-full-comparison-2026-10-29.parts.md)
contains raw logs, tests, scripts, patches, comparison analyses and checksums.

## Results

| Measurement | GNU Make | Openmktr Aggressive, fixed candidate |
| --- | ---: | ---: |
| Main build, including graph generation | 821.389751 s | 902.449351 s |
| Configuration through build, directly measured | 831.740044 s | 912.646027 s |
| Configuration | 10.28 s | 9.99 s (see JSON for full precision) |
| Repeat invocation | 5.330920 s | 86.302782 s |
| Main build mean CPU cores | 31.0730 | 29.2286 |
| Main build individual peak RSS | 0.727 GiB | 6.486 GiB |
| Main build sampled summed tree RSS | 10.765 GiB | 11.422 GiB |
| Repeat individual peak RSS | 0.216 GiB | 19.342 GiB |
| Maximum observed swap, main build | 0 | 0 |
| Maximum observed swap, repeat | 0 | 360 KiB |
| C++ compilation starts, clean / repeat | 9,341 / 0 | 9,341 / 0 |
| Installed file/symlink entries | 3,351 | 3,351 |

Openmktr's configuration-through-build total is 9.73% longer. Its repeat
invocation is about 16.19 times GNU's elapsed time, with approximately 89.6 times
the individual-process peak RSS. These are observed ratios from one pair, not
confidence intervals or hardware-independent claims. The failed first Openmktr
attempt is excluded. GNU remains the baseline to beat.

The host is an Intel Core i9-14900KF, with 32 logical CPUs and approximately
31.8 GiB physical RAM. WSL reports 28,734,660 KiB total after its 28 GiB limit.
The previous 24 GiB / 16 GiB swap / 32 CPU configuration is restored afterward.
"Maximum RAM" means equal access to this selected allocation; artificial memory
consumption is not added to either tool. Compiler flags remain the historical
release flags, rather than introducing a different optimization workload.

## Protocol and identities

The user selected one pair, the existing configuration, 32 jobs and a temporary
28 GiB WSL memory allocation (16 GiB swap). All work runs on the user's PC,
in Ubuntu 24.04 WSL's native Linux filesystem. The NAS is outside this campaign.
The prior WSL configuration is preserved in the campaign evidence directory.

The candidate starts at Git base `38ddf6d0e4c8bee9e5449992ed1c412205d6e56c`
plus the existing dirty working tree, including its runtime recipe fixes.
The campaign preserves the initial patch, file hashes and normalized source
snapshot. Existing historical evidence and build trees are retained.

LibreOffice archive SHA-256:
`21b38b7f429d305b229fb70f3294dd6ccd803f52dad0463d0923cdf143e9c8bd`.
Both lanes use Clang 18 and the identical historical configure options recorded
in JSON. Java, Skia, help and bundled dictionaries are excluded. This is the
complete build of that configuration. Compiler caches are disabled; filesystem
caches remain uncontrolled. GNU Make runs first.

The Openmktr executable is rebuilt with Chimera Clang, musl, libc++ and ThinLTO.
Its private dynamic interpreter and runtime permit execution in Ubuntu without
changing the project toolchain. Its banner says `unversioned` because the frozen
source copy has no Git directory; the binary hash and source manifest identify
the actual candidate.

## Timing and verification

Main-build timing includes the whole invocation, including Openmktr generation,
include remaking and helper compilation. A separate monotonic stopwatch covers
configuration through the completed build. Extraction, repeat builds, artifact
hashing, source integrity and runtime checks are recorded separately.

Resource samples occur every two seconds. Summed process-tree RSS can count
shared pages more than once and can miss peaks between samples. `wait4` peak RSS
is an individual-process high-water mark. Live process I/O and CPU tick samples
lose exited processes; final child user/system time is used for mean CPU cores.
These distinctions must be retained when interpreting the measurements.

## Fixes and improvement catalogue

Final findings will include reproduction commands, exact log references, source
locations, confirmed causes versus hypotheses, regression coverage and measurable
acceptance criteria. Earlier crashes and selected-target fixes are historical
evidence until exercised by this full build.

GNU's reference behavior expands recipes only for targets requiring work and
coordinates recursive concurrency through a shared jobserver. Those are useful
design criteria when examining expansion cost, dependency ordering and job-slot
accounting. See the [GNU Make manual](https://www.gnu.org/software/make/manual/make.html)
and [jobserver specification](https://www.gnu.org/software/make/manual/html_node/Job-Slots.html).
Recommendations will use original Openmktr implementations rather than imported
GNU implementation code.

### Implemented blocker: prerequisite-dependent wildcard expansion

The first full Aggressive attempt exited 1 after 337.329638 seconds, averaging
23.737 CPU cores. It used no swap. It completed approximately 7,200 Ninja actions
before failing on the two packaged liborcus libraries. This failed duration is
excluded from performance comparisons.

`solenv/gbuild/UnpackedTarball.mk` defines a generated-file recipe containing
`$(if $(wildcard $@),,$(call gb_Output_error,...))`. The external-project
prerequisite produces that library. At graph generation time the file does not
exist. Structural recipe inspection recognized shell and file functions but
omitted wildcard, so the emitted command permanently contained the error branch.
The libraries existed in the failed tree after prerequisite execution, confirming
that the error was stale rather than an absent compiler output.

The fix adds wildcard to the filesystem-sensitive functions in
`RecipeInspection::Expression` in `src/command.cc`. Existing deferred evaluation
then runs after prerequisites finish. A new focused test observes a file created
by a prerequisite. The measured baseline fails all three Ninja profiles with
`missing` instead of `ready`; direct mode and GNU Make pass. The candidate passes
all six combinations. This is a shared Ninja correctness defect, not specific to
Aggressive.

Two canonical snapshots change: `wildcard_cache.mk:test:ninja` and
`wildcard_with_commas.mk:test2:ninja` now print the prerequisite-created
`foo,bar`, replacing the old empty output. No crash expectation was added.
The remaining 856 snapshots match. The full Aggressive snapshot run matches
858/858 after recording and reviewing these two changes.

Local Chimera checks pass: C++ units, two version tests, nine focused recipe
tests, 180 correctness tests, 53 graph tests, six compatibility tests, 30 workspace
tests and seven jobserver tests. Aggressive correctness passes 180 tests.
ASan/UBSan passes the C++ units, nine focused tests and 180 correctness tests.
Formatting of `src/command.cc` passes. The include-dump smoke passes after
restoring executable permissions in the disposable source copy. Initial launcher
and permissions failures remain in the logs and are not counted as passes.

The fixed binary is `omktr-v2`; it has a separate hash and a fresh
`aggressive-v2` source and build tree. The GNU baseline is reused because the
compiler, configuration, host resource limits and workload remain unchanged.

## Artifact and application verification

All three original-source extractions (GNU, failed candidate, fixed candidate)
pass the 147,364-member content/symlink comparison. No third-party release
source was patched. Normalized `config_host.mk` SHA-256 is identical in all
three lanes: `9ae2b364bf43b50f6b7fc7211627fc451805958454ffd1be6f1e66d366d0be1a`.

There are no missing or extra installed entries. Of 3,351 entries:

- **3,137** match raw content or symlink identity.
- **195** ZIP-based templates, image archives, autocorrection and autotext
  files differ in archive representation, but their complete decompressed
  member maps are byte-identical. No XML or document text is discarded.
- **Nine** debugger scripts become identical after replacing only the lane's
  source/build prefix. Original raw hashes remain in the manifests.
- **Ten** shared libraries differ across the separate build paths. ELF section
  and string comparisons alone did not prove equivalence, so a GNU control
  rebuilt them at the exact Openmktr paths in an isolated mount namespace.
  All ten resulting libraries match Openmktr byte-for-byte. All **193** object
  files that differed across the initial lanes also match the same-path GNU
  rebuild byte-for-byte; the other **9,314** objects matched initially.

The control uses a separate copied build tree and a private bind mount. The
original measured tree remains intact. Its first direct-object attempt failed
because LibreOffice requires inherited library target flags; that attempt is
retained. The successful control requests `build`, restoring those scopes.
Control times are diagnostic and never enter the clean-build comparison.

Both applications report LibreOfficeDev 25.2.7.2. `ldd` succeeds for both
`soffice.bin` executables. Isolated profiles successfully produce PDFs containing:

- Writer: `Openmktr LibreOffice benchmark verification`.
- Calc: the `Name,Value` table and `Benchmark,42` row.
- Impress: `Openmktr Impress verification`.

The initial GNU Impress fixture omitted frame dimensions, giving an unnecessarily
narrow text box. GNU was retested with the same corrected fixture as the final
candidate. This is a fixture correction outside the timers, not a product fix.
The first harness also lacked WSL `pdftotext`; bundled `pypdf` was used to resume
verification. The already saved main-build and configuration-through-build
measurements remain valid.

These checks cover startup, three document pipelines, installed contents and
compiler outputs. They are not the full LibreOffice automated or GUI test suite.

## Remaining fixes and improvements, in priority order

### P1: stop repeated sidecar invalidation and unnecessary packaging work

**Evidence.** `aggressive-v2-noop.log` executes 26,025 Ninja actions and rewrites
20 installed image archives, despite zero C++ compilation starts. GNU's repeat
is 5.33 seconds; Openmktr's is 86.30 seconds. A subsequent execution-only dry run
still schedules 21,944 actions. `evidence/ninja-repeat-explain.log:1` explicitly
reports an output older than its `.kati_runtime-*` input. The first output is
`UnpackedTarget/dtoa-20180411.tgz`; subsequent examples include dragonbox and
static libraries. This establishes a concrete invalidation mechanism. It does
not establish that every scheduled action has the same cause.

**Implementation.** In `src/ninja.cc`, `EmitBuild` currently emits runtime
recipe/context sidecars as ordinary implicit inputs (around line 1394).
`MakeRuntimeRecipe` in `src/command.cc` includes the entire global-context hash
in recipe identity. Dependency-file population can change that context and
therefore many sidecar paths even when a recipe's effective inputs are unchanged.
Separate the stable recipe identity from unrelated global observations; retain
invalidation for variables the recipe can actually read. Make runtime guard rules
record successful unchanged-output checks using Ninja's restat semantics, and
verify whether that prevents stale sidecar timestamps from repeatedly scheduling
work. Inspect intermediate deletion as an additional independent source of dirtiness.
Do not simply make sidecars order-only: that could silently miss real command
changes. Fall back conservatively for computed variable names and effects.

**Regression/acceptance.** Add a producer/consumer fixture whose runtime guard
leaves an older output unchanged, then invoke the launcher twice. The second
invocation must schedule no real recipe. Add unrelated included-dependency and
global-variable changes that must not invalidate unrelated consumers, alongside
relevant-variable changes that must rebuild. On this LibreOffice configuration,
require no compiler, linker, image-archive rewrite or content change on repeats.
Use GNU's 5.33-second observed repeat as the initial performance target; establish
repeatability with new paired runs before claiming the target met.

### P1: reduce graph and re-evaluation memory

**Evidence.** `evidence/graph-size-analysis.json` measures the regenerated graph
at **753,053,731 bytes**. Dependency/build lines occupy 347,874,074 bytes,
`omktr_command` bindings 304,729,661 bytes and response contents 100,080,544 bytes.
One image-list response line is 11,651,052 bytes. The repeat reaches
20,281,808 KiB individual-process RSS, versus GNU's 226,348 KiB. The resource
trace records slight swap use only in Openmktr's repeat. Live allocation ownership
has not been profiled, so graph size is evidence of scale, not proof of every
allocation responsible for the memory peak.

**Implementation.** Profile parsing, dependency-node construction, expanded
command strings and graph emission separately. In `src/ninja.cc`, share common
launcher/command prefixes and write large response content into stable external
sidecars rather than repeating it in graph bindings. In `src/dep.cc` and the
generated-include path, deduplicate prerequisite storage and avoid duplicating
compiler-header observations already represented by Ninja depfiles. Preserve
ordering, automatic-variable semantics and regeneration dependencies. Prefer
streaming serialization over constructing several full graph-sized strings.

**Regression/acceptance.** Add a synthetic dense shared-header graph and a large
image-list response fixture. Check memory scales with unique dependency data,
not repeated textual prefixes. Run normal, sanitizer, regeneration and incremental
differential checks. Provisional targets on this machine: graph below 200 MB and
repeat RSS below 8 GiB, while preserving all output and invalidation checks.
These targets are proposals, not measured achievements.

### P2: reduce per-recipe process and context restoration cost

**Evidence.** Compilation samples typically show about 163 processes for
Openmktr versus 65 for GNU. The final graph directory contains 8,715 runtime
recipe sidecars and two approximately 26.7 MB contexts. `RunRuntimeRecipe` in
`src/command.cc` restores global definitions in a new evaluator for each deferred
recipe. Clean-build mean CPU use is 29.23 cores versus GNU's 31.07, and child
user+system time is about 26,377 seconds versus 25,523 seconds. These observations
motivate profiling; they do not assign the entire 81-second gap to this path.

**Implementation.** Measure decoder, expression restoration, symbol interning,
launch and shell costs in an isolated diagnostic run. Serialize the conservative
transitive variable closure for literal references; keep full context for
computed references. Consider an immutable shared decoded context with isolated
per-recipe scopes, or a worker service only if cancellation, exports, effects and
jobserver ownership can be preserved. Remove redundant shell layers where the
existing native launcher can directly execute the required interpreter.

**Regression/acceptance.** Cover target-specific origin/flavor, recursive macros,
automatic variables, runtime assignments, exports, failure cleanup, `.ONESHELL`,
parallel consumers and exact job limits. Demonstrate lower process/CPU cost in
the same fixture before rerunning a full clean pair. The initial end-to-end
performance objective is to beat GNU's 831.74-second configuration-through-build
result with equally verified outputs.

### P2: emit portable and faithful diagnostics

**Evidence.** Clean Openmktr output contains literal `-e [build CXX]` prefixes.
`src/func.cc` around lines 1026, 1038 and 1048 emits `echo -e` for deferred info,
warning and error functions. The selected POSIX shell prints `-e` literally.

**Implementation.** Replace those emissions with carefully quoted POSIX
`printf` using a fixed format and data argument. Preserve stream selection,
location prefixes, trailing-newline behavior and failure status. Do not interpret
user data as a printf format string or shell syntax.

**Regression/acceptance.** Compare direct and Ninja output for percent signs,
quotes, dollar signs, backslashes, leading `-n`/`-e`, tabs and newlines. Require
matching diagnostics under the reference shell and a second POSIX shell.

### P2: audit all filesystem-sensitive recipe functions

The implemented wildcard fix extends an existing shell/file deferral mechanism.
Audit `realpath`, native find and nested/computed calls for equivalent observations
of prerequisite-created paths. This is a follow-up audit, not a claim that those
functions are currently broken. Preserve the distinction between parse-time
evaluation and recipe-time observations; indiscriminate deferral can change
semantics or retain excessive context. Add missing-path, removed-path, changed
symlink and order-only-prerequisite tests, with GNU as the external oracle.

## GNU lessons and boundaries

GNU expands recipes after determining that the target needs updating, avoiding
both speculative filesystem observations and much unnecessary expansion. That
principle directly explains the fixed wildcard defect and motivates the repeat
work. Its shared jobserver provides one implicit slot per recipe and requires
exact return of additionally acquired slots. Preserve those contracts while
reducing launcher overhead; never improve CPU utilization by oversubscribing
recursive work.

The generated final graph contains 30,453 references to the selected immutable
`omktr-v2` pathname and zero `/usr/bin/make` references. `MAKE` is explicitly
assigned per lane. This is a graph/configuration audit, not an exhaustive syscall
trace of every third-party command. Third-party build recipes and the external
GNU comparison may use GNU tools. Openmktr itself remains built with LLVM, musl
and libc++; no GNU implementation code or new GNU runtime dependency is added.

Earlier variable-lifetime, cycle-scheduling, response-file and nested-read fixes
were already in the dirty starting tree. Their historical reports remain separate:
[earlier investigation](libreoffice-benchmark-2026-10-29.md) and its linked evidence.
This task adds the wildcard fix, regression/snapshot changes, reusable local runner,
release verifier and new report/evidence. No PR, push or merge is performed.

## Validation limitations and reproduction

Docker Desktop's Linux engine is unavailable (`dockerDesktopLinuxEngine` pipe
missing), so the Docker image gate and new CI runs were not performed. The local
Chimera results above apply to the frozen candidate, not a newly qualified
container image. Sanitizer coverage here is the units, focused runtime suite and
correctness suite; broader unchanged sanitizer suites are historical evidence.

Run a new pair with a new, empty lane directory and an immutable binary:

```sh
python3 validation/benchmark_libreoffice.py \
  --workspace /root/NEW-libreoffice-campaign \
  --report /path/to/local/report \
  --omktr /absolute/path/to/omktr \
  --jobs 32 --lanes gnu aggressive \
  --pdf-module-path /path/to/pure-python-packages
```

The runner is explicitly a local adapter: it uses the retained campaign archive,
download cache, Ninja and historical configuration. Its `--resume` option adds
new lane names without overwriting old trees. `--pdf-module-path` is optional
when pypdf is already importable. Existing lane directories are refused. Source
and binary hashes must be re-recorded after any fix. Full configure, bootstrap,
build and verification argument vectors are in the results JSON; they are more
authoritative than abbreviated command examples.

## Evidence

The native campaign directory is
`/root/libreoffice-pair-20261029-200435`. Its measured runner is `run.py`;
`driver.log`, `commands.jsonl`, individual command logs, resource JSONL files,
configuration snapshots and `report.json` record execution. Final repository
deliverables include the results and evidence links at the top of this document.
`evidence/log-index.json` maps important diagnostics to exact lines. In particular,
the first liborcus failure begins at `aggressive-build.log:11831`; the reduced
baseline mismatch is in `evidence/wildcard-baseline.log`; corrected tests are in
`evidence/fix-normal-gate2.log`, `evidence/fix-validation3.log` and
`evidence/fix-final-checks.log`. The two initial gate setup errors are retained.

Artifact evidence includes `installed-differences.json`,
`archive-payload-comparison.json`, `elf-section-analysis.json`,
`object-comparison.json`, `gnu-path-control2.json` and
`gnu-same-path-libraries.json`. Same-path control scripts and its failed first
attempt are retained. The evidence ZIP has an entry-level SHA-256 manifest and
an adjacent whole-archive checksum. Original full build trees remain on the PC.
