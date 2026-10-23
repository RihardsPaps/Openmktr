# Recursive child graph cache investigation

## Implemented follow-up

The first implementation is an opt-in **recursive no-op cache**. Enable it with
`omktr --recursive_cache -j32 all` or `omktr build --recursive_cache -j32 all`.
Children inherit the option. It preserves the direct executor: only an unchanged
child proven to need no recipes can return from the cache. Work, uncertainty or
unsupported features go through ordinary evaluation and execution.

The certificate records the invocation, physical working directory, selected
Makefiles, executable identity, complete environment (except live FIFO transport),
exact Makefile contents, and identities/existence of resolved graph nodes. File
identities include inode, size, mode, nanosecond mtime and ctime. Only simple
literal assignments/includes, explicit static rules and side-effect-free recipe
expansions are admitted. Generated includes, conditionals, functions, exports,
implicit rules, cycles, directories, phony work, and intermediate aggregates
are declined. This deliberately covers less than the proposed full graph cache.

`.openmktr-recursive.lock` serializes cache publication without waiting behind
a recursive parent in the same directory: lock contention falls back to normal
evaluation. Certificates are size-bounded, checksummed and published by atomic
rename. Missing, corrupt, obsolete and mismatched data are cache misses. A recipe
failure is returned normally and is never retried through a cache fallback.

The three original gaps were fixed independently of cache eligibility:

- Ninja stamps now use `KAT3` and file identities that detect preserved mtimes.
- Includes with producing rules force evaluation so their prerequisites are
  remade before reuse. Observable `info`/`warning` evaluation also forces it.
- Ordinary Ninja recipes use the shared FIFO through an internal supervisor.
  Recursive boundaries do not take an extra token. A nested Ninja borrowing a
  single reservation runs with one local job to avoid duplicating its ownership.
  Recipe stdout/stderr, custom shells, large response-file recipes, exit status
  and interruption handling retain their existing paths.

The historical investigation and unsafe measurements below are retained for
comparison; they describe the behavior before these fixes.

### Follow-up measurements on all 32 CPUs

The native cache was compared with the original `fa95018` executable, built
with the same Clang 22.1.8/musl/libc++ flags. The unchanged workload had 16
children, 30,000 assignments and four recipes per child. Seven samples were
measured after one discarded warmup, at both `-j8` and `-j32`. The container
affinity included all 32 logical CPUs. Source revision banners identify the
candidate as a working tree; the exact binary hashes and samples are retained in
[native results](recursive-cache-native-results.json).

| Jobs | Child workflow | Warm no-op | Child evaluations | Warm work | Peak recipes (median) |
| ---: | --- | ---: | ---: | ---: | ---: |
| 8 | Original direct baseline | 37.49 ms | 16 | 402.29 ms | 8 |
| 8 | Candidate direct, cache off | 36.69 ms | 16 | 396.68 ms | 8 |
| 8 | Native no-op cache | 8.02 ms | 0 | 405.55 ms | 8 |
| 32 | Original direct baseline | 30.32 ms | 16 | 149.20 ms | 32 |
| 32 | Candidate direct, cache off | 29.84 ms | 16 | 148.28 ms | 32 |
| 32 | Native no-op cache | 7.38 ms | 0 | 155.91 ms | 32 |

At `-j32`, the warm no-op is about 4.1 times faster (75.7% shorter) than the
original direct baseline, with no child Makefile parsing. Work falls back to
evaluation; its median here is 4.5% slower than baseline, reflecting certificate
validation/publication overhead and run variance. Cold time was 150.71 ms for
the enabled cache versus 147.68 ms for the original baseline. These are synthetic
measurements, not an overall compiler-build speedup claim or a confidence interval.

No native sample exceeded its job limit. The fixed explicit Ninja-child
experiment also stayed within the limit; see [Ninja-child results](recursive-cache-ninja-fixed-results.json).
The [standard benchmark comparison](recursive-cache-standard-bench.json) retains
conversion, regeneration, recipe-launch and ordinary execution measurements.
The three original semantic probes now report equivalence in
[fixed probe results](recursive-cache-fixed-probes.json).

Reproduce the native comparison with:

```sh
python tests/recursive_bench.py ./omktr --baseline /path/to/baseline/omktr \
  --cache-kind native --children 16 --variables 30000 --recipes 4 \
  --jobs 8 32 --repeats 7
```

The workload's invalidation samples remove outputs to compare equivalent work.
Separate correctness tests invalidate arguments, environment and included-file
contents with outputs still present, verifying cache misses without inventing
Make rebuilds. Regression coverage also checks source changes, missing outputs,
corruption, same-directory recursive lock contention, dynamic/export fallback,
large recipes, stdout/stderr and FIFO token conservation with inherited
reservations. The sanitizer build passed its three C++ unit binaries, all 142
correctness tests and six compatibility tests. Changed C++ formatting passed.
Twelve recorded snapshot changes contain only the printed jobserver wrapper
command; exit statuses, artifacts and all other output are unchanged.

A final clean Dockerfile build and portable run through Podman passed all three
C++ unit binaries, two version tests, 142 correctness tests, six compatibility
tests, 858/858 snapshots with zero quarantined crashes, and the include-dump
smoke check. The clean image executable's SHA-256 exactly matches the executable
in the native benchmark. Both benchmark-accounting tests and the changed-C++
format check passed. External project campaigns and remote CI were not run.

This investigation evaluates the proposed persistent child-graph cache without
changing normal recursive execution. The experiment explicitly invokes `omktr
build` in synthetic children, reusing the existing regeneration machinery. Child
graphs remain separate, and the parent still runs recursive recipes.

## Initial investigation decision

Do not automatically redirect recursive direct builds into `build` yet. Graph
reuse avoids repeated evaluation, but the existing regeneration stamp is not a
proof that skipping a direct evaluation preserves all observable behavior.
The next implementation should add a conservative eligibility check and stronger
dependency validation before enabling an automatic cache.

Three disposable probes demonstrate concrete differences in the current code:

| Input or behavior | Normal second evaluation | Reused child graph |
| --- | --- | --- |
| Parse-time `$(info evaluated)` | Prints again | Does not print |
| An included Makefile's prerequisite changes | Remakes include; result becomes `two` | Result stays `one` |
| Makefile contents change while its mtime is preserved | Result becomes `two` | Result stays `one` |

The generated-include case matters for ordinary dynamically generated rules; it
does not require an unusual timestamp manipulation. These are constraints on a
new automatic cache, not new compatibility guarantees for the explicit Ninja
workflow. The probes report differences rather than recording bugs as expected
test results. See [raw observations](recursive-cache-probes.json).

The larger scheduling probe also observed nested Ninja recipe oversubscription
at a requested `-j8`. Sharing a FIFO pathname does not make separate Ninja
schedulers acquire its tokens. That is an additional blocker to automatically
substituting Ninja child graphs.

## Existing machinery worth reusing

- `src/main.cc`: `Run` checks regeneration before evaluation. A hit bypasses
  Makefile-remaking logic as well as parsing. `HandleBuild` locks the recipe
  working directory through generation and Ninja completion.
- `src/ninja.cc`: `GenerateStamp` saves files, consulted environment variables,
  undefined variables, wildcard results, recorded commands and generation
  arguments. `GenerateShell` preserves recursive transport and recipe exports.
- `src/regen.cc`: `StampChecker` checks recorded inputs and command results.
  File checks compare mtimes against generation/check time, rather than content
  identities. Shell and file functions can have observable effects during checks.
- `src/file_cache.cc`: the parsed-Makefile cache belongs to one process. It does
  not avoid parsing in another child process.
- `src/fileutil.cc`: direct child executors acquire the shared FIFO tokens.
  Ordinary recipes across nested Ninja schedulers do not acquire that budget;
  any new cached executor must preserve reservation ownership and global limits.
  See [PR #38](https://github.com/RihardsPaps/Openmktr/pull/38)
  for the earlier scheduling measurements and endpoint validation.

## Proposed conservative cache contract

1. Evaluate the first invocation normally. Capture a versioned child execution
   graph plus its read set and the complete Makefile-remaking dependency closure.
   Store only after successful evaluation and eligibility checks. Do not execute
   a second speculative evaluation just to populate the cache.
2. Identify entries by executable/schema identity, physical working directory,
   selected Makefiles and include search paths, ordered targets, argument
   boundaries, assignments and origins, effective flags, recursion level, and
   consulted environment values including absence. Preserve target-specific
   exports and recursive MAKEFLAGS/MAKEOVERRIDES semantics. Use an exact manifest
   comparison after any index hash lookup.
3. Record content identities and existence states for Makefiles, includes, file
   reads and generator prerequisites; record wildcard results and relevant
   filesystem lookup outcomes. Changes must invalidate even with preserved or
   rolled-back mtimes. Instrument these reads; do not guess them from source text.
4. Initially decline stdin, arbitrary evaluation-time shell commands, observable
   diagnostics or file writes, recipe-time evaluation that mutates make state,
   untracked filesystem discovery, and generated Makefiles whose remaking cannot
   be validated. Treat unsupported modes and incomplete tracking as misses.
   Dynamic `eval` need not be banned forever, but its complete inputs and effects
   must be tracked before admitting it.
5. Validate under a per-working-directory lock. Publish a complete manifest and
   graph atomically; reject partial, corrupt, obsolete or mismatched entries.
   On a miss or uncertainty, run normal evaluation. Do not fall back after a
   recipe failure, since executing a recipe twice can change behavior.
6. Execute cached child graphs through the same scheduler and recipe-expansion
   semantics as direct children, using live FIFO state and one reservation per
   owner. Do not persist FIFO paths or reservations as configuration. Unconditional
   nested Ninja substitution would need its own equivalence proof.

Separate graph-affecting inputs from ordinary recipe inputs. Changing a source
file should normally schedule work from the cached graph; changing a generator
input that can alter rules must invalidate the graph. Missing information must
lead to normal evaluation, not a presumed hit.

## Reproduce the measurement

Run in Chimera with a binary built by the same toolchain as any comparison binary:

```sh
python tests/recursive_bench_test.py
python tests/recursive_cache_probe.py ./omktr
python tests/recursive_bench.py ./omktr \
  --cache-kind ninja \
  --children 16 --variables 30000 --recipes 4 \
  --jobs 1 8 32 --repeats 5 > recursive-cache-results.json
```

`--baseline /path/to/omktr` repeats the same experiment for another binary.
Without `--jobs`, the helper includes the process's full CPU affinity alongside
1 and 4 jobs. All conversion happens in disposable directories. The helper
checks artifact contents, exact child evaluation/reuse counts, and executed recipe
counts. It records observed overlap and flags job-limit violations without
discarding the evidence. Include,
argument and environment changes are tested separately; ordinary recipe-input
changes reuse child graphs while running work. Outputs are removed for these
probes to compare equivalent work in direct and Ninja modes.

The parent graph is generated before measurements; only children enable
`--kati_stats`. A legacy parent launcher restores its saved environment, so the
helper refreshes that parent outside the child timing when arguments or the
environment change. First child evaluation is measured separately. Each warm
case discards one warmup, then retains all five samples and their medians.

The occupancy metric integrates worker start/end intervals over wall time and
divides by the job limit. It is a lower bound on recipe slot use: it includes
the worker's sleep but excludes interpreter startup, shell setup, token wait,
parsing and regeneration. It is not CPU utilization or direct FIFO telemetry.
Peak overlap detects observed oversubscription, but cannot prove every token
transition correct. Add token wait/hold tracing before attributing real-project
performance changes to jobserver contention.

## Recorded run

On 2026-10-23, the experiment ran in the pinned Dockerfile's Chimera image using
Podman in WSL2. The host has an Intel Core i9-14900KF (24 cores, 32 logical CPUs).
With permission, WSL's processor limit was increased from 16 to 32 and WSL was
restarted; the container's affinity then included all 32 CPUs. WSL retained its
24 GiB memory and 16 GiB swap settings. The toolchain was Clang 22.1.8,
musl/libc++, Ninja 1.13.2 and Python 3.14.6. Runtime sources were unchanged from
`fa95018`; the container binary reports `unversioned` and its SHA-256 is retained
in the [raw results](recursive-cache-results.json).

The workload used 16 children, 30,000 assignments per child and four worker
recipes per child (64 independent recipes), each sleeping 30 ms. Both modes used
the identical binary and parent Ninja workflow, with direct mode measured before
cached mode at each job count. Results are medians of five measured samples:

| Jobs | Child workflow | Warm no-op | Child evaluations | Aggregate parse time | Warm work | Median peak recipes |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Direct | 220.54 ms | 16 | 92.51 ms | 3227.17 ms | 1 |
| 1 | Explicit cached Ninja | 52.17 ms | 0 | 0 ms | 3115.83 ms | 1 |
| 8 | Direct | 36.11 ms | 16 | 95.51 ms | 398.53 ms | 8 |
| 8 | Explicit cached Ninja | 11.41 ms | 0 | 0 ms | 139.84 ms* | 32* |
| 32 | Direct | 30.66 ms | 16 | 141.42 ms | 148.66 ms | 32 |
| 32 | Explicit cached Ninja | 8.63 ms | 0 | 0 ms | 110.69 ms* | 44* |

\* Oversubscribed: these work timings cannot establish a valid speedup under the
requested job budget. Median worker slot occupancy was 60.7% for direct `-j8`
and 172.9% for cached `-j8`; at `-j32` it was 40.7% and 54.6%, respectively.
A value below 100% can still hide oversubscription bursts, as the `-j32` peak
demonstrates. No-op samples executed zero child recipes in both modes. Their
timings show a possible benefit from eliminating evaluation, independently of
the work-timing violation. Parse times are summed across children and can exceed
wall time under parallelism.

The largest observed cached `-j32` overlap across the five warm-work samples
was 51 recipes; 44 in the table is the median of the per-sample peaks.

All measured samples checked artifact contents and execution/evaluation counts.
Separate include, argument and environment changes each caused 16 child
evaluations; ordinary recipe-input changes caused zero evaluations in cached
mode while executing all 64 recipes. This coverage does not address the unsafe
cases demonstrated by the separate probes. The first evaluations and raw samples
are retained, rather than hiding cold-start costs in the warm medians.

The workload deliberately stresses parsing and scheduling, using sleeping
recipes rather than compiler work. It does not measure CPU saturation, direct
token utilization, production hit rates, or Linux/FFmpeg build performance.
Mode order was fixed and no confidence intervals were estimated. A representative
real-project comparison and token tracing remain necessary before claiming an
overall build improvement.

## Initial investigation validation

The pinned Chimera image built successfully through the repository Dockerfile
using Podman (Docker Desktop's daemon was unavailable). Its portable command
passed all three C++ unit binaries, two version tests, 134 correctness tests,
six compatibility tests, 858/858 snapshot scenarios with zero quarantined
crashes, and the include-dump smoke check. The two benchmark-accounting tests
and a small direct/cached integration run also passed. The full measurement
completed all artifact, evaluation-count and invalidation checks; its scheduling
violations are reported above rather than classified as passing correctness.

During the initial investigation no C++ runtime code changed, and sanitizers,
external compatibility campaigns and real-project performance comparisons were
not rerun. The implemented follow-up and its sanitizer results are described
above. External project campaigns remain unrun for the implementation.
