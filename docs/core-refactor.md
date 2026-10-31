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
Pointer-sized allocation classes avoid padding small language objects; object
sizes and aligned payload starts preserve their required alignment. Ordinary
dependency nodes allocate double-colon rule groups only when needed.
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

`ready_queue` has one shared coordinator for outstanding prerequisites and reverse
edges, protected by a mutex. Completing participants update readiness directly
without passing through a separate completion queue. The calling
thread also executes ready tasks. Additional workers start only when runnable
work can overlap other ready tasks, and total participants stay within the job
limit. Pure metadata and literal phony no-op graphs create no worker threads.
Ready prerequisites are read together with their outcomes, and already-observed
source leaves need no second directory probe.
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
and misses. Scheduler worker counts describe spawned threads; peak active tasks
also include the calling thread. Filesystem timestamps and regeneration
checkpoints remain wall-clock
observations; elapsed timing uses monotonic clocks.

## Portable validation and performance

Runtime revision `fd7573bdd7e0166d3f97fa5d41e29b0e2613ae3f` is compared with the
original `77a397f` binary using the same pinned cloud Chimera toolchain. Both
normal and sanitizer binaries embed the recorded runtime revision.

All 28 portable checks pass, including 858/858 snapshots in each of the safe
and aggressive profiles, with zero quarantined crashes. Coverage includes
14 architecture cases, 187 correctness cases per profile, 45 runtime-recipe
cases, 58 graph cases, 30 workspace cases and 12 jobserver cases. Both builds
and all 17 ASan/UBSan checks pass. The session ownership/arena unit also runs
with LeakSanitizer enabled; the remaining sanitizer suites retain their
established `detect_leaks=0` setting. Whole-program leak detection is not claimed.
The external GNU incremental comparison passes all 27 scenarios without
incorrect outputs, unnecessary rebuilding or harness errors.

The no-regression acceptance checks pass all **60 timing and memory cells**:

- Five scenarios through direct, build and generated-shell entry points: 31
  alternating pairs per cell at `-j4`, with checked outputs and rebuild sets.
- Conversion, syntax checking, direct dry runs and no-op regeneration of a
  10,000-target graph: 101 alternating pairs per cell.
- Paired controls of the required `tests/bench.py` workloads: 31 pairs per cell,
  except the short no-op chain and serial recipe dispatch. Their initial
  near-parity medians led to one fixed extension to 301 pairs, retaining every
  earlier sample. Both extended cases pass.

Each case discards two initial warmup pairs. Every cell requires a paired median
candidate/baseline ratio at most 1.00 and a bootstrap 95% upper bound at most
1.05. Thresholds were not relaxed, and a faster case cannot offset a slower one.
The standalone `tests/bench.py --repeats 7` run is also retained; it measures
binaries in separate blocks and showed mixed raw medians. Alternating controls
resolve those differences without deleting the original measurements.

Selected ratios below use values above 1 for slower execution or greater memory:

| Workload | Median ratio | Bootstrap 95% interval |
| --- | ---: | ---: |
| Direct clean | 0.955 | 0.941–1.012 |
| Build clean | 0.918 | 0.912–0.924 |
| Generated clean | 0.914 | 0.906–0.932 |
| Direct no-op | 0.781 | 0.764–0.840 |
| Build no-op | 0.778 | 0.763–0.804 |
| Generated no-op | 0.983 | 0.895–1.016 |
| 10,000-target conversion | 0.904 | 0.890–0.921 |
| Conversion peak process RSS | 0.974 | 0.973–0.975 |

These results correct the earlier clean-build, startup, conversion and memory
regressions in the covered workloads. They are cloud synthetic measurements,
not real-project performance qualification. The RSS metric is `wait4` peak
individual-process memory; it is not aggregate tree memory, and small cases can
be masked by the parent/exec high-water floor.

The [performance report](../validation/evidence/core-performance-2026-10-31.json)
and [checksummed raw archive](../validation/evidence/core-performance-2026-10-31.zip)
retain binary hashes, revisions, drivers, individual samples, test logs, failed
attempts and rejected experiments. The [earlier architecture evidence](../validation/evidence/core-architecture-2026-10-31.json)
remains historical. In particular, it is not relabeled as passing performance
qualification.

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
