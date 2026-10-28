# Ninja graph optimization controls

`omktr build --graph_optimizations=safe` is the default. The `off` profile
disables the new optimization features. The `aggressive` profile additionally
considers recursive graph import, general statement replay, and an
already-running filesystem monitor. All profiles retain dependency validation.

Use repeatable `--disable_graph_optimization=FEATURE` options for comparisons.
Features are `evaluation`, `includes`, `recursive`, `compact`, and `monitor`.
Disabling a feature takes precedence over the selected profile. Generation
arguments and the executable identity invalidate graph reuse when settings change.
Profiles and explicit feature exclusions also reach opaque recursive Openmktr
children. Child command-line profiles override the inherited profile; inherited
feature exclusions still apply. The private `KATI_GRAPH_SETTINGS` channel keeps
`MAKE` as one executable pathname and avoids adding tool-specific options to
`MAKEFLAGS`. It is excluded from Make's environment-variable bootstrap and
reserved against Make export materialization. An unknown transport version
disables optimizations unless a valid command-line profile selects them.
Compact emission places the common native recipe launcher in a shared rule and
keeps each quoted script, cleanup arguments and depfile in edge bindings. Large
response files and outputs requiring special handling retain ordinary emission.
`--kati_stats` reports effective settings, region hits/misses, effect barriers,
include certification and recursive-import decisions. Existing phase timers
separate parsing, evaluation, include remaking, dependency resolution and emission.
Ninja launcher jobserver setup preserves existing paths and retries name
collisions with a bounded suffix search. Other FIFO creation failures report the
underlying diagnostic and terminate before Ninja starts.

## Evaluation and generated includes

The evaluator reconstructs statements and scopes on every evaluation pass. It
reuses validated pure expression results, including results used inside dynamic
names, conditionals, calls, loops and `eval`. Cached records contain structural
expression identities, variable definitions and origins, and owned result bytes.
Reads retain their evaluation order. Validation looks up existing symbols without
interning stale dependency names, including on invalidation. Recursive-variable
appends participate in identity checks. Derived call and loop bindings have
scoped identities; simple automatic bindings include their current values. Diagnostics,
visibility restrictions and external effects prevent reuse of enclosing regions.
Tracing modes retain ordinary evaluation.

Safe mode concentrates reuse on string/list transformations. Aggressive mode
also replays normalized statement programs for assignments, rules, recipes,
conditionals, includes, exports, undefine and VPATH. Each program combines an
owned structural source identity, input binding observations, ordered operand
expansions and a certificate of the mutations it produces. Replay validates each
operand at its original execution point and applies mutations through the normal
evaluator APIs to a fresh bootstrap state. Origins, precedence, flavors,
target-specific bindings, exports, rule records and secondary expansion therefore
retain the ordinary evaluator's behavior. Conditions and includes walk their
current child regions, and dynamically parsed `eval` bodies record independent
programs. Definitions from obsolete branches never survive into a new invocation.
An invalid operand is evaluated and recorded in place; recovery never restarts
an invocation after effects have executed. Computed recipe automatic variables
whose observations are not represented by scalar records, and other unsupported
bindings or operations, continue through ordinary evaluation at the affected
boundary.

Pattern-variable scopes and their definition ordering are indexed during
dependency resolution. Adding scopes through dynamic evaluation refreshes the
index. This avoids scanning every explicit target-variable entry for every
dependency while retaining pattern specificity and append ordering.

Recorded arbitrary shell operations and file writes require ordinary evaluation
in every profile, rather than a separate speculative shell-check pass. This prevents
executing those effects twice when a regeneration check would otherwise discover
a changed result. Explicit existing no-rerun extensions retain their semantics.
Native file reads record identities, including missing files and changes that
preserve mtimes. A missing-file observation accepts only `ENOENT`; later path
errors or unreadable dependencies require ordinary evaluation. Wildcards retain
both matches and empty queries. Recognized
native find queries are revalidated by the native interpreter in an isolated
worker; validation never executes a shell inferred to be pure from its text.
The worker accepts only complete, quiet results, including current search
observations. Unsupported syntax or diagnostics force ordinary evaluation.
Replayed reads register their current regeneration dependencies, and a replayed
find restores `.SHELLSTATUS`. Arbitrary shell and file effects still execute
exactly once; unchanged downstream pure values can be reused afterward.

Dependency resolution shares filesystem observations only within its current
epoch. Serialized direct-mode recipe expansion also retains observations across
pure checks, including repeated automatic-variable prerequisite scans. A shared
process epoch brackets real commands and filesystem writes across executor
threads. Active effects force fresh probes; completion invalidates retained
metadata before the next expansion. `--kati_stats` reports recipe metadata hits,
misses and conservative fallbacks. This retains Ninja's own dependency checking.
Regions that already overlap active effects use ordinary probes throughout,
avoiding per-probe cache bookkeeping. Such bypasses invalidate any enclosing
certificate and are counted separately in the statistics.
Ordered shell and file effects invalidate live wildcard and native find
queries as well. Earlier wildcard snapshots remain available for include loops
and validation. Different results for the same pattern within one invocation
require ordinary evaluation rather than accepting a flat stamp.
A generated-include certificate
is admitted only after a complete no-work include-remake pass, with unchanged
dependency observations. Certificates bind producing sources, inputs, missing
search candidates, generated graph and environment files. Phony work, VPATH,
unresolved graphs and unsupported features retain ordinary evaluation.

Cache payloads are bounded, versioned and checksummed, with atomic publication.
The main regeneration stamp seals all structural fields; only its fixed-size
validation checkpoint may be refreshed independently. It also binds the Ninja
graph, launcher and environment file to the accepted generation. Nonregular
cache files, including FIFOs, are rejected without blocking.
Missing, obsolete, corrupt or mismatched records are misses. Internal evaluation
and include records use `.kati_stamp*` names inside the graph output directory.
Keep using `omktr build` for its existing build-directory lock.

## Recursive import

Aggressive mode can import silent standalone `$(MAKE)` invocations with static
arguments and explicit child graphs. A child process preflights source definitions
before evaluation. Effectful discovery, generated source includes, implicit rules,
custom execution constraints and uncertain invocations retain ordinary recursion.
Child recipes keep their working directories, exported environment and Make level.
Parent prerequisites gate imported recipes before they can execute.

Ownership is checked before committing imports. Symlinked output paths, hard
links and unresolved parent directories retain ordinary recursion. Duplicate
outputs, cycles, or an
opaque recursive sibling cause conservative fallback for the parent graph. This
first implementation therefore does not import part of a graph alongside opaque
recursive siblings. General Kbuild and shell-loop flattening are not supported.
Target-specific depfile metadata currently falls outside the static preflight;
automatic depfile detection, parent pool assignments and `.WAIT` boundaries also
retain ordinary recursion. Incompatible job budgets, target-specific `MAKEFLAGS`,
special execution modes, load constraints, precious or ignored child outputs,
observable evaluation effects and unresolved environment expansions also retain
ordinary recursion. Children producing discovery diagnostics use ordinary
recursion so speculative parsing cannot duplicate their messages. No generic transitive dependency reduction
is performed.

The direct executor also lets dependency waiters run queued work when shared
prerequisites occupy other workers. Borrowed tasks have a lower dependency rank
than every active task and owned node on that thread. This prevents an acyclic
graph from acquiring a false same-thread dependency cycle through helping.
Ranks merge representations of the same output ownership; cyclic ownership
graphs and deep helper stacks retain ordinary waiting. Recipe admission,
`.NOTPARALLEL`, recursive serialization and the shared FIFO budget remain in
force. This executor fix applies in every profile, including `off`, so feature
ablations use the same scheduler on both sides.

FIFO admission uses one local reader at a time. A released inherited reservation
wakes that reader through a separate Linux event descriptor; the notification
never enters the token FIFO. The reader retries reservation ownership before
waiting for capacity held by other processes. This prevents an idle inherited
slot from leaving all queued recipes blocked. Systems without the wake descriptor
retain bounded retries and the existing interruption checks.

## Content-preserving output generation

The stdout producer participates in the inherited shared job budget, including
when the helper is invoked inside an opaque recursive build. An inherited
reservation is reused. Interruption while waiting for admission leaves the
previous output intact and does not start the producer.

Use an explicit stdout-producing command:

```sh
omktr write-if-changed config.h -- python3 generate_config.py
```

The helper writes a sibling temporary file, compares bytes and atomically
publishes changed content. Identical output retains its mtime. Existing regular
file permissions are preserved; new files use the normal umask. Producer stderr
and failure status remain visible. Failure leaves the prior output intact and
removes the temporary file. Symlink and directory outputs are rejected.

Pair this recipe with `.KATI_RESTAT: config.h` to let Ninja discard downstream
work when the generator produces identical content. Recipes are never rewritten
into this contract automatically. Producers should write the result to stdout,
not modify the destination themselves.

## Optional Linux filesystem monitor

Run these commands from the build working directory:

```sh
omktr monitor start
omktr monitor status
omktr build --graph_optimizations=aggressive
omktr monitor stop
```

Starting installs directory watches and reconciles events before reporting ready.
Builds never start a daemon implicitly. The monitor caches batches of file
identities between inotify events; it brackets snapshots with event checkpoints.
Environment inputs and files outside the monitored workspace retain ordinary
checks. Ninja continues its own dependency scanning.

Watch loss, event overflow, restart, uncovered paths, ambiguous aliases and
unsupported filesystems fall back to ordinary validation. Eligible files must be
regular, have one link, and have an unambiguous path within the watched tree.
The initial filesystem allowlist is ext4, XFS, Btrfs and tmpfs; overlay and network
filesystems fall back. Stop and restart a monitor whose status requires fallback.
Socket and lock files use the reserved `.openmktr-monitor.*` names.

## Validation and performance status

Run `python tests/graph_optimizations.py` alongside the normal portable and
sanitizer suites. The tests cover effects, dynamic evaluation, retained
timestamps, cache corruption, include certificates, ownership conflicts,
prerequisite gates, exports, monitor lifecycle and output publication.

The agreed real-project acceptance requires positive paired 95% intervals for
clean, no-op and single-source builds of FFmpeg 7.1.1 and Linux 6.12 at j8/j16
through `omktr build`. Other tested modes and scenarios must show no supported
regression. The full performance objective remains **unverified** until the
recorded matrix passes. Exploratory samples are not acceptance results.

On 2026-10-27 the user replaced the large benchmark campaign with a maximum of
20 further build invocations. The bounded runner
[`benchmark_graph_budget.py`](../validation/benchmark_graph_budget.py) counts
configuration commands and previously consumed slots, reserves a slot before
launch, and stops on failure without automatic retries. Its fixed matrix uses
18 commands: two shared configurations, clean baseline/candidate pairs for both
projects at j8/j16, and no-op/single-source pairs for FFmpeg j8 and Linux j16.
Both variants receive identical copies of the prepared configuration before
release compilation. These single-pair results are descriptive; they cannot
establish the original seven-pair confidence gates. The full statistical matrix,
legacy measurements, runtime sweep and feature ablations are outside this
reduced campaign.

The bounded campaign completed with all 20 slots consumed and all correctness
and integrity checks passing. Its original Linux j8 clean pair was 26.36%
slower; the final diagnostic candidate repeat remained 18.86% slower than that
baseline. This is an unresolved performance concern. It is retained alongside
the FFmpeg and Linux j16 observations in the
[published evidence summary](../validation/evidence/README.md). No full
performance acceptance or exclusion of regressions is claimed.

An earlier candidate's default-profile FFmpeg 7.1.1 comparison passed through `omktr build`
with Ninja 1.11.1. Each job count has two discarded warmup pairs and seven
alternating measured pairs. These are median paired percentage improvements;
brackets give the seeded bootstrap 95% interval. These historical results are
superseded by the state-replay changes and do not qualify the current candidate:

| Scenario | j8 | j16 |
| --- | --- | --- |
| Clean | 6.45% [5.44, 6.50] | 5.39% [5.03, 6.20] |
| No-op | 80.37% [79.97, 80.72] | 81.54% [81.38, 81.62] |
| Single source | 81.83% [81.74, 82.17] | 81.50% [81.07, 81.74] |

Missing-object and graph-change scenarios also have positive intervals. Artifact
hashes, required rebuilds, unchanged unrelated outputs and the FFmpeg smoke test
pass; source contents remain unchanged. Baseline and candidate are static builds
using identical Clang 22/musl/libc++ flags; external release compilation uses
Clang 18 for both. Candidate identity is the binary/source manifest, rather than
the unchanged generated revision string. See
[the machine-readable results](../validation/graph-optimization-results.json)
for exact identities and raw evidence references.

Overall acceptance is **incomplete**. The Linux/Clang 18 run stopped at a compiler
frontend crash while compiling `mm/slub.c`; three subsequent controlled standalone
compiles pass, and baseline/candidate post-failure preprocessed input hashes match.
The intermittent crash's cause remains unresolved. A fresh Linux/Clang 19 campaign
on a native Docker volume stopped after two measured pairs when the WSL/Docker
runtime restarted. Its container exited 255 without an OOM-killed flag. Neither
Linux run has seven measured pairs or qualifies at j8/j16. Their failed and
partial evidence is retained. Source-integrity checks pass, accounting for the
archive link normalization performed by Python's initial `data` extraction filter.

A subsequent Clang 19 run failed in the unchanged baseline during its first
discarded warmup, before any measured pairs. Its exact compiler invocation,
400 independent frontend probes and a fresh GNU Make control build passed.
This does not establish the intermittent failure's cause. Historical attempts
on another builder are excluded from current acceptance. All current project
work and qualification take place on the local PC, with no NAS access. New
measurements are kept separate from every previous host, binary and failed
attempt.

General statement replay and native-observation validation are implemented;
their current qualification is recorded separately from the historical results.
The current `b7b75e45` binary passes the full portable and required sanitizer
gates, changed-file formatting, all 53 focused graph tests, 576 seeded state
comparisons, and all 27 external incremental scenarios in both safe and
aggressive modes at j1 and j4 on the local PC using the Chimera image. Both
static binaries were rebuilt on the PC with identical compiler flags. The portable
gate includes 858 regression scenarios in each profile. These checks establish
correctness coverage; they do not satisfy the real-project performance gates.
The preceding `09b0f15d` binary completed Linux builds and integrity checks at
both job counts, but its clean-build intervals were inconclusive. Those
measurements are historical. The current acceptance matrix runs entirely on the
PC. A captured WSL kernel panic interrupted its Docker campaign; fresh
measurements use an independently qualified local Linux VM and native ext4
directories, with the same CPU availability for both variants. The VM remained
healthy when Clang 19.1.1 crashed compiling `net/wireless/pmsr.c` in the first
task after three measured pairs. Its saved preprocessed input and original
driver command pass 72 independent serial and parallel retries with stable
output hashes. The cause remains unresolved; this run is incomplete and does
not qualify any performance gate. A full Linux build through external GNU Make
is the next independent control. Every earlier failed or inconclusive run
remains separate. FFmpeg uses the same private NASM runtime for both variants
and retains assembly support and the original release sources.
Aggressive-mode real-project qualification, legacy-entrypoint comparisons,
real-project feature ablations, separate Ninja-runtime comparisons and monitored
cold/warm performance qualification were not completed and are excluded by the
reduced campaign scope. Passing functional suites and FFmpeg observations do
not establish those original gates.

Extend `validation/benchmark_ninja.py` invocations with, for example,
`--candidate-option=--graph_optimizations=aggressive`. Options are recorded in
metadata and exact command journals. Follow the campaign guide before using
external releases; do not modify their sources to obtain a passing result.

Use `--monitor --candidate-option=--graph_optimizations=aggressive` to measure an
explicitly started monitor. Its startup is included in each candidate clean
invocation and recorded separately in the phase data. Following no-op and
incremental scenarios retain that monitor; shutdown and artifact verification
are outside the timers. `--ninja-dir=DIR` selects one Ninja runtime for both
variants and records the supplied runtime bundle's file hashes.
For incremental feature ablations, `--retained` initializes separate build trees
once and then measures no-op, single-source, missing-object and graph-change
pairs. These reports are explicitly marked as retained state and contain no
clean-build intervals. Initialization, including explicit monitor startup, is
retained in the command journal outside those intervals. Warmups are still
discarded, measured pairs still alternate, and each sample keeps distinct logs.
`--toolchain-dir=DIR --llvm-version=19` selects a private external LLVM
executable directory for both variants and records its wrapper and manifest
hashes. It does not change Openmktr's GNU-free build toolchain.

## Current dependency qualification (2026-10-27)

The dependency refresh uses distribution Clang 22.1.8 and Ninja 1.13.2 on the
local PC. It has its own persistent 200-command ledger, separate from the
exhausted historical 20-command campaign. Original sources, failures and earlier
results remain intact. The protocol is **one discarded warmup pair and seven
alternating measured pairs**, comparing explicit safe/aggressive profiles of the
same binary and toolchain. Compiler caches are disabled and configurations cloned
for each pair. See the [dependency results](../validation/dependency-upgrade-results.json)
and their raw evidence for identities, timings, command reservations and limits.

FFmpeg 9.0.2 passes artifact, rebuild-set, configuration, direct-repeat, smoke and
source-integrity checks for the selected configuration. Its six measured scenarios
all show a supported regression. Median paired improvements and bootstrap 95%
intervals are:

| Scenario | j8 | j16 |
| --- | --- | --- |
| Clean | -1.40% [-2.16, -1.23] | -2.03% [-2.41, -1.99] |
| No-op | -59.84% [-62.72, -57.58] | -68.49% [-69.56, -67.14] |
| Single source | -79.43% [-83.49, -76.70] | -80.41% [-86.97, -78.80] |

Linux 7.3's statistical matrix is incomplete. The external environment initially
lacked flex's correctly named m4, GNU sed's binary substitutions, and a working
fixed-date conversion for initramfs timestamps. The campaign image fixes those
prerequisites without editing release sources. Focused fresh Linux correctness
checks use the same ledger and do not substitute for performance measurements.
Those initial checks did not establish a fix for the historical Linux performance
regression. The separate follow-up below records later comparisons.

The twelve-scenario promotion gate has failed. **Safe remains the default**;
aggressive, off and individual exclusions remain available. No filesystem monitor
starts implicitly. Other refreshed current releases remain pending qualification;
their historical passes do not transfer to new releases.

## Linux and Alpine follow-up (2026-10-27–28)

The [follow-up report](../validation/dependency-followup-results.json) preserves
the preceding reports and archives. Alpine's workspace failures are fixed with
native timestamp comparisons and a distribution Zstandard command backend when
Python omits `_zstd`. All 29 workspace and 53 graph tests pass on Alpine, Gentoo
and Chimera. The final Chimera binary also passes the complete reference and
ASan/UBSan gates. Linux process supervision now waits on a lazily opened process
descriptor after stdout closes, retaining timed fallback on unsupported systems.

The Linux comparison rebuilds the unmodified historical baseline revision
`58bb206` and the candidate with identical updated Chimera compiler/linker flags.
Both compile unmodified Linux sources using Alpine's supported Clang 22.1.3 in
the same pinned comparison image. This is a candidate regression comparison;
it does not isolate safe versus aggressive behavior within one binary. A failed
Chimera compiler attempt and its successful standalone reproducer remain separate.

Linux 7.3 j8 completes one discarded warmup pair and seven alternating measured
pairs with matching artifacts, configurations, rebuild sets and source contents:

| Scenario | Median paired improvement | Paired bootstrap 95% interval |
| --- | ---: | ---: |
| Clean | -0.08% | [-1.25%, +0.31%] |
| No-op | +23.04% | [+18.73%, +25.21%] |
| Single source | +3.67% | [+3.50%, +5.84%] |

The same candidate's j16 clean result is -0.55% [-0.73%, -0.26%], a supported
regression. Its j16 no-op and single-source intervals improve by +4.78%
[+2.43%, +9.87%] and +1.78% [+1.04%, +5.37%], respectively. This candidate fails
the strict improvement gate. All its direct/Ninja correctness checks pass.

A subsequent allocation fix retains callers' owned path strings during timestamp
cache lookup. It preserves the cache's mutation invalidation and C-string/bounded
view semantics. The isolated million-query benchmark improves by 20.56% on Chimera
and 29.45% under Alpine's runtime; its same-binary control shows no supported
difference. These synthetic results do not establish release qualification.
The new candidate passes the complete reference, Alpine, Gentoo and sanitizer
gates, including additional native and sanitized path-probe edge tests.

That allocation-only cohort was interrupted after 13 reservations when the next
investigation identified excessive scheduling of recipe-less dependency leaves.
Its incomplete samples are retained and do not count toward qualification.
The executor now checks those leaves inline while continuing to schedule nodes
with recipes or dependencies. Ownership, mutation invalidation and dependency
rank protections remain in force.

The Alpine overlap gate then exposed a pipe inheritance race: a parallel recipe
could inherit a sibling's pipe between `pipe()` and `fcntl(FD_CLOEXEC)`, keeping
its writer open after the intended child exited. Linux recipe pipes now use
[`pipe2(O_CLOEXEC)`](https://man7.org/linux/man-pages/man2/pipe.2.html).
A regression test widens the former race and checks children for inherited
sibling pipes; it fails on the preceding binary and passes with atomic creation.
The original failed gate and captured process/descriptor state are preserved.
The fixed binary passes the complete Chimera, sanitizer, Alpine and Gentoo gates,
all four incremental comparison lanes, and 10,000 repeated overlap iterations.

Its first three current-Linux j8 clean pairs improve by -0.283%, -0.072% and
+0.349%. With two negative pairs already recorded, even hypothetical +99%
improvements in all four remaining pairs leave the prescribed bootstrap lower
bound negative. This cohort was explicitly interrupted at reservation 262 to
avoid spending the remaining allowance on a binary that cannot meet that
criterion. It is **unqualified**, with no completed seven-pair result claimed.
The samples, early-rejection calculation, interruption and unchanged-source
check remain intact. Separate reserved clean-build CPU profiles investigate
the remaining overhead; their instrumented timings are excluded from acceptance.

The subsequent reservation fix has a direct reproducer: with no free FIFO tokens
and one inherited slot, four queued recipes formerly stalled after the first
completed. They now finish with peak concurrency one and no FIFO token added.
All seven jobserver/process cases pass, including a separate run with eventfd
deliberately unavailable. A synthetic recursive contention workload improves by
36.11% [+34.70%, +37.52%] using seven alternating pairs after one discarded
warmup. Its artifacts and token return are checked. These are mechanism results;
the separate release comparisons below determine qualification.

The historical Linux 6.12 comparison completes one discarded warmup pair and
seven measured alternating pairs using the supported Clang/LLVM 20 historical
image. Its median paired clean-build improvement is +0.17%, with a positive
95% interval of [+0.09%, +0.29%]. All artifacts match and source contents are
unchanged. This is a new toolchain/timing variant, separate from the preserved
LLVM 19 execution-only evidence. Current Linux 7.3 still uses Clang 22.1.3.

The final Linux 7.3 matrix completes seven alternating measured pairs after one
discarded warmup pair at each job count. The final binary is
`8027d04f6693a10801536d4e17da008b98595eb42875a652ee3e93eefbd240cf`.
All artifact, configuration, rebuild-set and source-content checks pass, as do
the separate default-safe direct clean/repeat checks. Paired improvements and
bootstrap 95% intervals are:

| Scenario | j8 | j16 |
| --- | --- | --- |
| Clean | +0.31% [-0.58%, +1.05%] | +0.52% [-0.07%, +0.76%] |
| No-op | +29.16% [+27.47%, +39.00%] | +32.87% [+25.20%, +36.86%] |
| Single source | +7.98% [+5.54%, +8.19%] | +7.69% [+6.36%, +8.15%] |

Both clean intervals cross zero. Neither establishes a supported regression or
the required positive improvement, so the strict six-scenario Linux performance
gate **does not pass**. Four incremental/no-op scenarios pass. These comparisons
also do not isolate safe/aggressive features within one binary or replace the
failed initial FFmpeg profile comparison.

The interrupted pre-update Docker cohort and its samples remain separate. A new
independent source check matches all 96,080 entries to the official archive using
the original Python data-filter extraction policy; the first raw-link comparison
is retained with its symlink-normalization limitation documented. No interrupted
timings were transferred to the complete restart.

**Safe remains the default.** The same durable ledger ends at **394 of 400**
authorized commands, leaving six unused; no reservations were reset. The
[follow-up report](../validation/dependency-followup-results.json) and
[separate evidence archive](../validation/evidence/dependency-followup-2026-10-28.zip)
retain completed, failed and interrupted cohorts. No full campaign or
aggressive-default qualification is claimed.
