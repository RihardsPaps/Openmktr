# Recursive no-op cache

## Behavior and eligibility

The opt-in **recursive no-op cache** skips validated unchanged children. Enable it with
`omktr --recursive_cache -j32 all` or `omktr build --recursive_cache -j32 all`.
Children inherit the option. It preserves the direct executor: only an unchanged
child proven to need no recipes can return from the cache. Work, uncertainty or
unsupported features go through ordinary evaluation and execution.

This cache does not store compiled outputs for a new package version's working
directory. The separate [cross-version compiler-cache investigation](cross-version-reuse.md)
records the completed ccache experiment while retaining normal build evaluation.

The certificate records the invocation, physical working directory, selected
Makefiles, executable identity, complete environment (except live FIFO transport),
exact Makefile contents, and identities/existence of resolved graph nodes. File
identities include inode, size, mode, nanosecond mtime and ctime. Only simple
literal assignments/includes, explicit static rules and side-effect-free recipe
expansions are admitted. Generated includes, conditionals, functions, exports,
implicit rules, cycles, directories, phony work, and intermediate aggregates
are declined. VPATH, MAKEFILES and includes resolved through a search path also
fall back until absent search candidates are tracked. This eligibility boundary keeps uncertain builds on ordinary evaluation.

`.openmktr-recursive.lock` serializes cache publication without waiting behind
a recursive parent in the same directory: lock contention falls back to normal
evaluation. Certificates are size-bounded, checksummed and published by atomic
rename. Missing, corrupt, obsolete and mismatched data are cache misses. A recipe
failure is returned normally and is never retried through a cache fallback.

Related safety guarantees apply independently of cache eligibility:

- Ninja stamps use `KAT4` and file identities that detect preserved mtimes.
- Includes with producing rules force evaluation so their prerequisites are
  remade before reuse. Observable `info`/`warning` evaluation also forces it.
- Ordinary Ninja recipes use the shared FIFO through an internal supervisor.
  Recursive boundaries do not take an extra token. A nested Ninja borrowing a
  single reservation runs with one local job to avoid duplicating its ownership.
  Recipe stdout/stderr, custom shells, large response-file recipes, exit status
  and interruption handling retain their existing paths.

## Measurement and reproduction

The synthetic 16-child, 30,000-assignment, four-recipe workload measured warm
no-op time of 7.38 ms with the native cache versus 30.32 ms for the original
direct baseline at j32 (seven samples after one warmup). Work fell back to
evaluation and measured 155.91 ms versus 149.20 ms. This is invocation overhead
evidence, not an overall compiler-throughput claim.

A later eligibility-proof reuse optimization removed repeated source scans
during work. On its separate 15-sample comparison, cached warm work was
152.21 ms versus 149.21 ms uncached; cached no-op was 7.49 ms. Dirty graphs
still evaluate and execute normally, and publication rereads source bytes.

```sh
python tests/recursive_bench.py ./omktr --baseline /path/to/baseline/omktr \
  --cache-kind native --children 16 --variables 30000 --recipes 4 \
  --jobs 8 32 --repeats 7
```

Use `--baseline-cache` with a supporting prior cache binary to compare cache
implementations. Use `--cache-kind ninja` for explicit Ninja children. Run with
matched toolchains and record the available CPU affinity.

## Retained evidence

- [Native implementation results](recursive-cache-native-results.json) and
  [optimized results](recursive-cache-optimized-results.json): samples, identities,
  invalidation checks and phase timers.
- [Fixed semantic probes](recursive-cache-fixed-probes.json),
  [Ninja-child job limits](recursive-cache-ninja-fixed-results.json) and
  [standard overhead comparison](recursive-cache-standard-bench.json).
- [Initial investigation](recursive-cache-results.json),
  [original probes](recursive-cache-probes.json) and
  [optimization measurements](recursive-cache-optimized-results.json):
  historical observations; use the implemented contract above for current behavior.

The initial proposed full child-graph cache was superseded by the narrower
no-op cache. Historical unsafe behavior is not a supported cache contract.
Current validation commands are in the [contributor guide](../CONTRIBUTING.md).
