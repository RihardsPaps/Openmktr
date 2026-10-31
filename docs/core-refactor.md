# Core runtime refactor: first delivery

This document tracks the staged speed and reliability overhaul from baseline
`77a397ffd7a1cc5f48e406f8cf9052a50cc0d150`. It is not a claim of GNU Make parity
or completion of the complete architectural plan.

## Implemented runtime boundaries

- `process.h` owns shell execution, output transport, signal forwarding and
  process cleanup. A launch failure returns a failed recipe status, including
  under `-k`, rather than exiting before returning the shared reservation.
  Partially constructed launches own their descriptors, spawn structures,
  temporary scripts and children. Infrastructure failures after launch reap
  the child before returning its reservation or ending the filesystem-effect
  interval. Failed output spooling stops the recipe, removes its changed
  non-precious output and lets independent `-k` work continue.
- `jobserver.h` owns shared FIFO reservations and Ninja load admission.
  `JobLease` returns its reservation on ordinary and exceptional exits;
  per-thread FIFO connections close when their worker exits.
- Filesystem utilities retain timestamp/identity observations, invalidation
  epochs, globbing and recipe-state storage. Recipe-state reads reject
  nonregular files without blocking and bound input size.
- `ready_queue.h` schedules a resolved task graph. One coordinator maintains
  outstanding prerequisites and reverse dependents; workers receive only ready
  tasks and never wait on another task's dependencies. Source leaves, empty
  rules and already-current outputs can finish in the coordinator. A bounded
  batch keeps workers supplied between short recipes without adding threads.
  Literal phony no-op recipes use the normal command expansion once in the
  coordinator; custom shells still go to workers, and failed prerequisites
  prevent expansion. This preserves export effects without dispatching dummy
  commands that the process runner can finish immediately.

The executor retains a separate serial traversal for `-j1`, explanation and
`.NOTPARALLEL`. Parallel graph preparation preserves first-visit target scope,
records pre-prerequisite output timestamps, and drops circular edges in stable
traversal order. Missing intermediate decisions belong to individual parent
edges, not globally completed outputs. Prerequisite lists used for automatic
variables remain unchanged even when task dependencies are deduplicated.

A single preparation worker can overlap expansion and completion bookkeeping
with admitted recipes. The local recipe admission limit and inherited shared
jobserver remain authoritative; the extra worker does not increase the local
recipe limit. Evaluation-time shell functions retain their existing admission
behavior.
Evaluator mutation remains serialized. This stage does not make the evaluator
reentrant or introduce a persistent shared mutable evaluator.

## Diagnostic interface

Use `--stats_json FILE` to append one JSON object per reporting process to a
JSON Lines file. For example, run from a disposable project directory:

```sh
omktr -j8 --stats_json /tmp/omktr-stats.jsonl all
omktr build -j8 --stats_json /tmp/omktr-stats.jsonl all
```

Schema 1 includes `pid`, `clock`, `peak_rss_kib`, `phases`, `counters` and
`statistics`. The `build` supervisor and graph-generation child append separate
records. The option does not propagate to arbitrary recursive Make commands,
and its filename does not alter the graph's regeneration identity.

Durations use `CLOCK_MONOTONIC`; filesystem freshness and regeneration
checkpoints retain wall-clock timestamps. Phase times and accumulated statistic
durations can overlap and must not be added into an end-to-end total. Scheduler
node, worker and inline-task counters accumulate across include-remake and goal
passes; `scheduler_peak_active` is the maximum number of executing worker tasks.
Process-launch counts cover the shared command runner, not arbitrary children
spawned inside recipes. No raw environment values or expanded recipes are
included in the JSON. Existing `--kati_stats` diagnostics remain available.
Fatal errors that terminate before reporting are retained in the benchmark's
command status and logs; a missing statistics record never indicates success.

## Reproducible measurements

`validation/benchmark_ninja.py` now accepts `direct` alongside `build` and
`legacy`. With `--baseline-kind make`, the baseline is external GNU Make while
the candidate retains the chosen Openmktr backend. All generation and
regeneration work is included in the build timer. `--release-version` selects
and verifies an archive against the reviewed dependency campaign manifest.

A GNU comparison requires an external comparison environment; GNU Make is not
a dependency of the portable runtime or test image. For example:

```sh
python validation/benchmark_ninja.py --project synthetic \
  --root /disposable/parity-report \
  --baseline /path/to/gmake --baseline-kind make --baseline-revision VERSION \
  --candidate /path/to/omktr --candidate-revision REVISION \
  --entrypoints direct build --jobs 8 --warmups 2 --samples 7
```

Reports retain failed command evidence, warmups, individual paired samples,
output checks and source-integrity checks. A timing cell qualifies only with
at least seven complete pairs, a median candidate/baseline ratio at most 1.00,
and a bootstrap 95% upper bound at most 1.05. Whole-report qualification also
requires every requested cell and sample and no command or integrity failure.
This timing result alone does not establish the three-project completion gate
or the independent memory nonregression requirement.

The LibreOffice harness takes explicit `--archive`, `--configuration`, `--cc`,
`--cxx`, `--make` and optional `--ninja-dir` inputs. Its configuration JSON must
contain the recorded `configure_options`. It supports `gnu`, `direct`, `safe`
and `aggressive` lanes and rejects resume when configuration, archive, compiler
environment or Openmktr identity differs. The LibreOffice harness still reports
its existing clean/repeat/application checks; its full paired incremental
parity matrix remains outstanding.

## Validation of this delivery

The [machine-readable report](../validation/evidence/core-runtime-refactor-2026-10-31.json)
and [evidence archive](../validation/evidence/core-runtime-refactor-2026-10-31.zip)
retain tool and binary identities, paired samples, failed baseline tests, local
normal/sanitizer logs and the external GNU incremental comparison. The measured
runtime revision is `e21e75c2c23422f94b313708e7c98627586677db`.

The local sanitizer gate passes, including 187 correctness tests in both
profiles, 45 runtime recipe tests, 58 graph tests, 30 workspace tests and 12
jobserver tests. The external incremental campaign reports no correctness
blocker, unnecessary-rebuild defect or harness error. Local snapshots match
856/858 cases in both profiles; the two filesystem enumeration-order differences
are byte-identical with the baseline. GitHub's portable and graphical gates
also passed for this runtime revision; workflow links are in the report.

The synthetic performance gate is **not passed**. At four jobs, the final
seven-pair run improves direct no-op time by 14.2% and missing-output rebuilds
by 6.2%, but clean direct builds regress 5.6%. Other cells remain uncertain;
Ninja missing-output regressions also appear in the preceding paired runs.
The separate 21-pair, 2,000-phony-no-op test improves by 26.5% (95% bootstrap
interval 23.5–29.0%) after fixing unnecessary worker dispatch. These are paired
median gains, not whole-project speedups. The archive retains the earlier
no-op regression and all subsequent measurements. Memory and real-project
qualification on the user's PC remain outstanding.

## Remaining architectural work

The accepted overhaul is larger than these runtime changes. The following
remain required before describing it as complete:

1. Invocation-owned symbols, variables, parsed files, diagnostics and evaluator
   state, with multiple-session isolation tests and explicit arena ownership.
2. A shared typed graph/freshness contract for direct execution, Ninja runtime
   guards and explanations, including replacement of timestamp sentinels, and
   unified admission of recipe and evaluation-time processes.
3. Generation-wide atomic publication, reader pinning, recovery and automatic
   versioned-state migration. Individual generated files are still published
   by the existing writer.
4. Unified observation records and selective persistent dependency-resolution
   reuse, preserving ordered Makefile effects and bounded memory/work budgets.
5. Complete pinned LibreOffice, Linux and FFmpeg correctness, memory and timing
   qualification in both modes on the user's PC, followed by integration and
   release documentation for the resulting formats and behavior.

Historical performance reports remain historical. Cloud synthetic measurements
must not be presented as those project-level results.
