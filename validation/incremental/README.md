# Differential incremental correctness

This external campaign compares GNU Make, direct Openmktr, and generated Ninja
from equivalent disposable source copies. It supplements `tests/correctness.py`,
`tests/make_compat_regressions.py`, and the release-build campaign. GNU Make is
installed only in an external comparison image; the portable image and runtime
remain GNU-free.

The [results](RESULTS.md) record the original interruption recovery failure and
the passing serial and parallel reruns after its fix in both Openmktr modes.

```sh
docker build -t openmktr-test .
docker build -f validation/incremental/Dockerfile -t openmktr-incremental .
mkdir incremental-evidence
docker run --rm -v "$PWD/incremental-evidence:/evidence" openmktr-incremental
```

Or on a POSIX campaign builder with Python, Clang, GNU Make and Ninja:

```sh
python3 validation/incremental.py --omktr /path/to/omktr \
  --output /path/to/new-evidence-directory
```

`--project explicit --case shared-header` selects a focused run. Run the full
manifest at `--jobs 1` and the default `--jobs 4` when qualifying a release.
Every invocation requires a new output directory. All generation takes place
inside copied trees. Original source content hashes are verified after each case.

The checked-in projects are representative **fixtures**, not third-party release
compatibility claims: an explicit C dependency graph, a compiler-depfile graph,
and a recursive Makefile graph. Each covers no changes, one changed C source,
a shared header, a generated header's input, a newly discovered header dependency,
a deleted object, tracked flags, an included Makefile's graph change, and recovery
after interruption. Flags are tracked by `flags.mk` prerequisites: GNU Make does
not automatically rebuild merely because a command-line variable changed.

Each scenario has an independent warm baseline. The runner regenerates with
the same arguments and `--regen` before every Ninja invocation, then executes
`sh ninja.sh`. It observes target hashes, nanosecond mtimes, sizes and inodes,
so rewriting identical bytes is still recorded as work. Controlled input edits
advance beyond existing timestamps and wait for wall time, including coarse
timestamp filesystems. It compares target sets without relying on parallel log
ordering, checks compiler depfile coverage, checks settled no-op builds, and
compares deterministic artifacts against a fresh GNU Make build of the mutated
sources. The clean oracle catches invalid reuse even when both incremental
engines miss the same dependency.

Interruption is synchronized with a marker written **after** a recipe damages an
object. The runner sends SIGINT to the build's entire process group, waits for
termination, and kills any remaining descendants. Recovery work is measured
separately from work completed before interruption. There is no arbitrary
interrupt timing or modification to production sources to inject a failure.

## Evidence and release policy

`report.json` records selection, tool paths/versions/digests, the explicit build
environment, source hashes, target sets, artifact hashes, dependency coverage,
and defect categories. `commands.json` and per-phase logs preserve commands,
directories, statuses, timing, generation and execution output. Trees and
per-case `result.json` files remain available for diagnosis.

| Exit | Meaning | Release policy |
| --- | --- | --- |
| 0 | Selected checks passed | Evidence applies only to the selected scope |
| 1 | Missed rebuild, stale/missing deterministic output, missing dependency, or failed recovery | **Block release** |
| 2 | Incomplete run, failed baseline/oracle, missing tool or other infrastructure error | **Block release; validation incomplete** |
| 3 | Only unnecessary rebuilding | Performance defect; report separately |

CI runs the complete fixture manifest; release publication depends on this
check. No baseline snapshots or known-failure exemptions suppress missed rebuilds.
A baseline no-op that recompiles is a performance defect, even if output bytes
are unchanged. A successful exit from a build alone is never acceptance evidence.

## Add real campaign projects

Use `--manifest path/to/projects.json`. Its top-level `projects` list can share
fields through `defaults`; project fields override defaults. Source paths resolve
relative to the manifest. Point each project at a pristine pinned source tree,
not an existing build directory. Do not copy absolute build paths or cache files
from prior builds. No original tree is edited. Controlled scenario edits apply
only to disposable copies and must not be used to repair third-party sources.

Each project supplies:

- `name` and `source`, identifying the pinned configuration and pristine tree.
- `prepare`: optional arrays of command arguments for identical configuration in
  each copy. These commands must not compile project outputs or depend on tree
  location. Use the same compiler, linker, flags, environment, and targets.
- `args`: build arguments (default `["all"]`), including any `-f` selection,
  assignments and targets. `{make}`, `{omktr}`, `{cc}`, and `{python}` expand
  in command arguments. `--env NAME=VALUE` records explicit environment overrides.
- `artifacts`: exhaustive relative glob patterns for recipe-produced outputs to
  compare, including generated headers, objects, libraries and executables. The
  set must exclude graph files, caches and depfiles. Empty baselines fail.
- `deterministic`: optional subset of those patterns for byte comparison;
  defaults to all artifacts. Establish determinism before excluding artifacts;
  document exclusions in the campaign record. At least one hash check is required.
- `dependencies`: depfile paths mapped to required prerequisite paths. The current
  parser accepts Clang depfiles with line continuations and whitespace-free paths.
  Escaped spaces and other dependency formats need an adapter before use.
- `cases`: named scenarios with `changes` and optional exact required `expected`
  target lists. Changes use `path` and `action` (`write` with `text`, `replace`
  with unique `old`/`new`, or `delete`). `prime` optionally changes inputs and
  warms new dependencies before the measured edit. Case `dependencies` can
  override expected depfile coverage after graph changes.
- `interrupt`: optional `arm` and `marker` paths for a project-specific synchronized
  failure injector. Supply this only when the build supports observable recipe
  synchronization. A missing marker fails validation; it never silently skips.

Instrument projects whose recipes restore output mtimes: filesystem observation
cannot detect a byte-identical rewrite that also restores inode and timestamp.
Dependency coverage checks only declared probes; completeness depends on an
exhaustive artifact list and representative mutations. Do not infer coverage of
all external campaign projects from the bundled fixtures. Pin source/archive
digests and configuration in the external campaign record, qualify each adapter,
and retain a full nine-scenario report per representative release configuration.

The GNU-free harness self-tests inject missed work and excessive work and check
that the clean oracle catches stale outputs:

```sh
python3 tests/incremental_campaign.py
```
