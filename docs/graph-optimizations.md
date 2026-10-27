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
This does not establish the intermittent failure's cause. Qualification has
moved to an independent native Linux AMD builder using the same pinned compiler
image and release archives. That builder also reproduced the baseline Openmktr
binary byte for byte with the pinned Clang/musl build image. New measurements
are kept separate from every previous host, binary and failed attempt.

General statement replay and native-observation validation are implemented;
their current qualification is recorded separately from the historical results.
The preceding `09b0f15d` binary passes the portable and sanitizer gates, 576 seeded state
comparisons, and all 27 external incremental scenarios in both safe and
aggressive modes at j1 and j4 on the independent AMD Linux builder. These checks
establish correctness coverage; they do not satisfy the real-project performance
requirements. A storage-capacity interruption during discarded warmups requires
a fresh performance matrix on a separate native ext4 disk.
The interrupted run and its evidence are retained separately.
Aggressive-mode real-project qualification, legacy-entrypoint comparisons,
real-project feature ablations, separate Ninja-runtime comparisons and monitored
cold/warm performance qualification also remain outstanding. Passing functional
suites and FFmpeg default results do not satisfy those remaining gates.

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
