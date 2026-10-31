# Core speed and reliability architecture

This overhaul starts at `77a397ffd7a1cc5f48e406f8cf9052a50cc0d150`.
The implementation and portable validation are separate from release performance
qualification: the remaining release gate is the paired PC campaign described
below. Historical project results do not qualify this revision.

## Invocation and language ownership

`BuildSession` owns symbols, global bindings, flags, parse/file caches, diagnostics,
filesystem observation state, statistics, native-find state and runtime snapshots.
A session allocation arena owns expressions, statements, rules and variables.
Aligned 64 KiB chunks keep allocation ownership and live-object bitmaps out of
individual objects; size-class free lists recycle early deletions. Oversized
objects use separate blocks. Teardown destroys survivors before their backing
text and services, and sanitizer builds poison unused and released slots.
Thread-local service pointers are borrowed caches, invalidated whenever the
binding changes; the session still owns and publishes each service exactly once.
Scoped worker bindings share the invocation without sharing
mutable state with another invocation. Evaluator mutation within one invocation
remains serialized. Process-wide signal handlers still have one CLI owner.

Fatal diagnostics unwind through the invocation boundary. The ready scheduler
cancels active process work before joining workers on an expansion failure.
Children are reaped, partial non-precious outputs are removed, and reservations
are returned before cleanup finishes. The session unit exercises 100 sequential
sessions, nested sessions, independent threads, evaluator calls and worker
bindings with LeakSanitizer enabled.

## Execution and freshness

`ready_queue` has one coordinator for outstanding prerequisites and reverse
edges. Workers receive only ready tasks. Source leaves, current outputs and
literal phony no-ops can finish in the coordinator; runnable batches are bounded.
The serial traversal remains for `-j1`, explanation and `.NOTPARALLEL`.
Preparation preserves first-visit target scope, pre-prerequisite target stamps,
stable cycle removal and edge-specific missing-intermediate decisions.

`process` owns spawn structures, descriptors, output transport, signal forwarding,
reaping and cleanup. `jobserver` owns local process admission, inherited FIFO
reservations and load admission. Evaluation-time shell functions and recipes
now enter the same local process budget. A waiting process holds no shared FIFO
token. Session teardown closes cached worker connections and wakeup descriptors.

`FileStamp` distinguishes missing, ignored, present and logically updated nodes.
Present timestamps use exact seconds and nanoseconds, including dates before the
Unix epoch. `NodeOutcome` distinguishes running, succeeded and failed execution.
Direct execution, explanations, automatic `$?` and native Ninja freshness guards
use the shared freshness predicates. Order-only and validation edges do not
make an output stale. Recipe-time queries reobserve files after effects while
retaining logical/phony updates. Recipes needing deferred automatic freshness
use the native evaluator, so shell quoting cannot turn `$?` into a literal shell
variable. Existing dialect restrictions remain documented in the compatibility
guides; this refactor does not add a new Make dialect.

## Publication, readers and migration

Each output-directory/suffix pair has a writer lock and an immutable bundle
under `.omktr-generations/`. Graph, environment, launcher, stamp and context
manifest are completed and synced before a single `current` symlink rename
publishes them. Public filenames remain entry points. A checksummed versioned
manifest validates bundle artifacts; the stamp separately seals its structural
bytes while allowing its validation checkpoint to advance.
Immutable object writes use batches of at most 256 paths, syncing every file
and each containing directory before publication. Unsealed reused paths
participate too, so an interrupted or concurrent writer cannot supply an
unsynced object to a committed graph. After flushing, objects become owner-read-only
(`0400`); this seal permits later reuse without repeated disk barriers, while
full bytes are still checked. Sealing precedes recording regeneration identities.
These private cache files must remain immutable; regenerate after editing them.

Generated launchers pin their selected bundle with a shared lease for the whole
Ninja run. A native supervisor owns the lease, jobserver FIFO and load state,
forwards cancellation, reaps Ninja and then removes its resources. FIFO creation
and token seeding require no external shell utilities. A checksummed launch plan
stores literal exports, unsets and Ninja options in the same published bundle;
the supervisor validates it before applying the environment. `omktr build`
launches this plan directly, while `ninja.sh` is a short native-launch entry point.
Collection checks
exclusive leases, retains the current and preceding
bundles, and visits at most 256 candidates per publication. Interrupted staging
directories are recovered by later maintenance. Legacy public files migrate
automatically through a preserved legacy snapshot. Corrupt, obsolete or missing
state triggers regeneration rather than requiring manual cache deletion.
Caller-created regular Ninja wrappers, such as pool declarations including the
generated graph, remain supported.

Recipe expected states, scripts, variable contexts and environment snapshots
are immutable objects. Filename hashes select storage; full bytes establish
identity, and atomic no-replace publication resolves collisions. Mutable success
history is separate from immutable expected state. Small expected-state records
share bounded packs (at most 256 records and 1 MiB). Each record has a stable
private symlink descriptor naming its pack and byte range; readers parse these
bytes without following the descriptor as a path. Bounded regular-file reads
check the record's digest, and publication compares the complete record bytes.
Pack contents and descriptor directories join the publication durability barrier.
Changing one record leaves neighboring command identities intact. Legacy regular
state objects remain readable.

Raw Ninja reads retain their
open graph inode and refer to stable immutable recipe objects, including
intermediate-state proofs; they do not depend on a collected bundle directory.
Shared objects are retained for saved graphs and raw Ninja consumers. Remove them
only with their owning disposable workspace when all readers have stopped;
automatic bundle collection intentionally does not guess which saved graphs
still reference objects. Normal `omktr build` validates and regenerates stale
formats, including runtime-recipe schema v7. Old standalone generated graphs
should be regenerated when updating the executable.

## Selective persistent resolution

Typed observation records cover file contents/identity, metadata, glob results,
native-find results and ordered symbol interning. Existing evaluation records
and the resolution cache share decoding and validation. Positive and negative
filesystem observations invalidate equally. Any overlapping filesystem effect
or epoch change makes an observation log ineligible for reuse.

The `resolution` optimization persists pure top-level implicit-chain feasibility
queries. Its identity includes ordered rule structure, suffix precedence, phony
names and executable identity. It validates every witness before replaying symbol
interning in order. Recipe text and target-variable changes can retain these pure
queries; scopes, secondary expansion and Makefile effects still execute through
the ordinary resolver. VPATH and unsupported query contexts take the ordinary
path. It never replays a persistent mutable evaluator or a complete resolved graph.

Budgets are 16 MiB stored, 64 MiB validation/recording work, 8,192 cache entries,
and 1 MiB/8,192 observations per query. Oversized identities, invalid records,
publication failures and budget exhaustion fall back to uncached resolution.
`--disable_graph_optimization=resolution` provides an ablation; `off` disables it.
`safe` and `aggressive` enable it independently of evaluation memoization.

## Diagnostics

`--stats_json FILE` appends schema-1 JSON Lines with process identity, monotonic
phase durations, peak individual-process RSS, counters and accumulated statistics.
No raw recipes or environment values are included. Phases can overlap and must
not be added into a wall-time total. The build supervisor and generation child
report separately. Missing records on fatal exits never count as success.
Existing `--kati_stats` diagnostics remain available, including resolution hits
and misses. Filesystem timestamps and regeneration checkpoints remain wall-clock
observations; elapsed timing uses monotonic clocks.

## Portable validation

The pinned cloud Chimera toolchain passed all 28 portable gate commands and all
19 sanitizer build/test commands at architecture revision `7fdb573`. Both safe
and aggressive snapshot profiles pass 858/858 scenarios. The full suites include
187 correctness cases per profile, 45 runtime-recipe cases, 58 graph cases,
30 workspace cases and 12 jobserver cases. The external GNU incremental campaign
reports no correctness blocker, unnecessary rebuilding or harness error.

The final runtime revision `62a7200` adds missing immutable environment-object
revalidation and batches writes before sealing durable objects. Its normal and ASan/UBSan
rechecks pass 10 architectural contracts, 58 graph cases, 45 runtime-recipe cases
and 187 correctness cases. Session
teardown also passes with LeakSanitizer enabled. The ordinary sanitizer suite
uses its established `detect_leaks=0` setting; an optional broader leak-check
attempt stopped in the LeakSanitizer tracer itself; an isolated retry passed.
This does not establish a whole-program leak result. The evidence retains the
failed attempt, the corrected cancellation-test and sealing order, and successful
rechecks. The final normal recheck also runs as an unprivileged user.

The [architecture report](../validation/evidence/core-architecture-2026-10-31.json)
and [checksummed raw archive](../validation/evidence/core-architecture-2026-10-31.zip)
record exact revisions, commands, samples and limitations. They supplement the
historical first delivery; they do not qualify release-project performance.

Seven alternating cloud pairs at `-j4` retain correct outputs and rebuild sets,
but do **not** demonstrate speed parity. Ratios below are medians of paired
candidate/baseline times; values above 1 mean slower:

| Synthetic workload | Median ratio | Bootstrap 95% interval |
| --- | ---: | ---: |
| Direct clean | 1.069 | 1.035–1.120 |
| Generated clean | 1.190 | 0.995–1.224 |
| Generated Makefile change | 1.215 | 1.150–1.333 |
| Direct no-op | 1.025 | 0.901–1.067 |
| Generated no-op | 1.272 | 1.118–1.498 |

A separate seven-repeat phase benchmark reduces parallel dispatch of 2,000
phony no-op recipes from 17.01 to 12.04 ms (29.2%), while conversion of 10,000
targets rises from 168.77 to 219.92 ms (30.3%). Conversion peak process RSS rises
from 27,556 to 32,620 KiB. Batching and sealing avoid the earlier repeated-flush
regression; all preceding measurements remain in the archive. These mixed
results and the unrun PC campaign keep release performance unqualified.

## Remaining PC qualification

[`qualify_core.py`](../validation/qualify_core.py) runs the reviewed Linux 7.3,
FFmpeg 9.0.2 and LibreOffice 26.8.2.2 releases against external GNU Make in both
direct and build modes. The [configuration example](../validation/core-qualification.example.json)
and [run guide](../validation/README.md#qualify-the-core-overhaul-on-the-pc)
cover tools, archives, configure options and incremental probes.

Each requested job count uses two discarded warmup pairs and at least seven
alternating measured pairs: clean, no-op, source touch, missing output and
Makefile timestamp change. Checks retain configurations, artifacts, source
integrity, rebuild sets, application output, command failures and individual
samples. LibreOffice additionally verifies Writer, Calc and Impress PDF output.
No third-party sources are patched. Incomplete evidence cannot qualify.

Every timing and memory cell requires a median candidate/baseline ratio no worse
than 1.00 and a bootstrap 95% upper bound no worse than 1.05. The memory metric is
`wait4` peak individual-process RSS, not aggregate simultaneous process-tree RSS;
LibreOffice also retains sampled tree RSS for diagnosis. Qualification requires
all projects, modes, job counts and scenarios. Cloud synthetic checks exercise
the implementation but cannot establish this PC release gate.

The [first delivery evidence](../validation/evidence/core-runtime-refactor-2026-10-31.json)
retains its measured runtime revision, earlier failed experiments and limits.
It remains historical and is not relabeled as final architecture qualification.
