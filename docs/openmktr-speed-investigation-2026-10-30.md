# Openmktr speed investigation on LibreOffice

The largest confirmed opportunity is evaluation memoization overhead. A disposable
variant that disables memoization while retaining pattern-variable indexing cuts
fresh LibreOffice graph preparation from **90.27 s to
34.63 s**. The **55.64 s** phase saving is equivalent
to **44.1%** of the latest reported 126.111-second clean-build gap.
It is not a measured reduction in a completed full build.

The existing Safe profile also generates this project's graph faster than
Aggressive. Runtime context restoration is the next measured CPU target.
Graph path compaction reduces storage and Ninja loading memory, but the tested
form does not improve loading time.

[Structured results](../validation/openmktr-speed-investigation-results.json)
and [checksummed evidence](../validation/evidence/openmktr-speed-investigation-2026-10-30.zip)
retain commands, individual samples, diagnostic patches, scripts and logs.
The [previous full comparison](libreoffice-improvements-2026-10-30.md) remains
the authoritative completed build comparison: GNU 834.112 s, Openmktr 960.223 s.

## Test conditions and boundaries

All benchmark workloads ran on the user's PC, in native WSL Linux storage. Every build invocation
requested 32 jobs; the runtime fixture used 32 workers. WSL temporarily had 30 GiB
RAM and 16 GiB swap, leaving about 1.8 GiB of the host's physical memory for Windows.
Serial evaluation cannot use all 32 workers merely because the job limit is 32.
Workloads ran sequentially. No NAS was accessed.

The measured baseline is the exact reported binary, SHA-256
`e551c7fa3f7e6056d39ebdf864b64cdae1a8ec29d7ae81b3b4451694d0da1d59`. The diagnostic uses the same Chimera Clang 22.1.8,
musl, libc++, O2 and ThinLTO toolchain. Its sole behavioral source change is
forcing `IncrementalEvaluator::enabled_` false in a disposable source copy.
The working C++ files still match the frozen baseline after CRLF normalization.
No production optimization, commit, push or PR was made.

Generation controls use private copy-on-write overlays of the retained build.
Fresh preparation uses two new build directories and a read-only mount of the
original release source. Normalized configurations match. Original-source
verification checks 147,364 files/symlinks with zero differences. The retained
original graph's SHA-256 is unchanged. Timers use CLOCK_MONOTONIC_RAW; guest NTP
was temporarily stopped for the generation and standard benchmark controls, then
restored. The original 24 GiB WSL configuration was restored byte for byte and
confirmed active at final inspection.

## Evaluation caching is losing on this workload

The fresh preparation pair includes initial evaluation, generated include work,
the required restart, dependency resolution and graph emission. It excludes the
main Ninja application build, configuration and bootstrap setup timers. Each
configuration completed the same visible include-remake work: one C compilation,
one link and 303 dependency actions.

| Fresh graph preparation | Current Aggressive | Memoization disabled with index retained |
| --- | ---: | ---: |
| Complete generation invocation | 90.270 s | 34.629 s |
| First Makefile evaluation | 27.993 s | 3.705 s |
| Evaluation after include restart | 28.271 s | 2.755 s |
| Include execution before restart | 13.473 s | 13.230 s |
| Final dependency resolution | 4.431 s | 4.600 s |
| Final Ninja generation | 13.440 s | 8.435 s |

Scoped rows can overlap; do not sum them to reconstruct the total. The candidate
ran first, with uncontrolled filesystem caches. This is one preparation pair,
not a confidence interval or a completed LibreOffice speed result.

On the retained, populated build, the repeated generation controls were:

| Variant | Initial invocation | Repeat 1 | Repeat 2 | Repeat mean |
| --- | ---: | ---: | ---: | ---: |
| Current Aggressive | 81.47 s | 71.60 s | 70.32 s | 70.96 s |
| Current Safe | 59.00 s | 49.83 s | 49.73 s | 49.78 s |
| Diagnostic with index retained | 44.41 s | 36.59 s | 36.30 s | 36.44 s |

Safe's repeat mean is 29.9% below Aggressive;
the diagnostic's is 48.6% below Aggressive.
These are generation-only results. They do not establish faster complete Safe
builds or verified no-op invocations.

The profiled Aggressive generation recorded 76,544 region hits and 3,758,065
misses, approximately a 2% hit rate. In
[incremental_eval.cc](../src/incremental_eval.cc), `Begin`, `AppendScopeIdentity`,
`Fingerprint`, `Read` and `StateRead` construct keys and copy binding identities
while trying to reuse those regions. The experiment attributes a large phase cost
to the memoization subsystem collectively; it does not individually time each
of those functions.

**Do not use `--disable_graph_optimization=evaluation` as the proposed fix.**
[DepBuilder's pattern-variable index](../src/dep.cc) uses that same feature flag.
There are only two pattern scopes among 13,725 target scopes in this build.
Disabling the feature makes dependency resolution scan the much larger set for
every target. That control was stopped after 165.05 s while still resolving
dependencies. The diagnostic separates the two mechanisms and keeps the index.

## Runtime recipes repeatedly restore a large context

The fresh graph contains 4,356 deferred recipes, of which **3,454** reference a
roughly 32.64 MB global context. If each deferred recipe runs once, the references
represent **112.83 GB** of context reads. This is calculated logical input volume,
not physical disk traffic or a syscall trace; the page cache can serve the bytes.

A separate fixture invokes 256 trivial deferred recipes with 32 workers, using
the unchanged binary. The large variant loads the real retained 32.74 MB context
with 4,574 definitions; fixture definitions override its relevant bindings. Four
batches per variant alternate order. All 2,048 timed outputs contain the expected
text. Median results are:

| Context | Wall time per 256 recipes | Aggregate child CPU time |
| --- | ---: | ---: |
| Small fixture context | 0.103 s | 0.50 CPU-s |
| LibreOffice context | 1.527 s | 44.88 CPU-s |

This isolates context loading, restoration and trivial evaluation, without the
production supervisor and successful-read certificate publication. Its ratio is
not a full-build speedup. Separate instrumented runs confirm substantial time
in metadata reading and variable restoration before evaluating the tiny recipe.

[RunRuntimeRecipe](../src/command.cc) eagerly restores every global definition.
[ReadGraphCache](../src/graph_optimization.cc) reads, copies and checksums the
complete payload. The next prototype should validate the context once, index
definitions, and restore values lazily when used. Preserve computed references,
origin/flavor, exports, automatic variables, local eval effects and corrupt-state
rejection. A shared decoded-context design needs equally strict per-recipe
isolation; a persistent worker service is not the first change to attempt.

## Graph compaction helps memory rather than this speed gap

An offline prototype replaces repeated build/source path prefixes on Ninja
build lines with Ninja variables. Expanding each replacement reproduces its
original line byte for byte. Ninja successfully parses both graphs and emits
identical complete target listings, including order, in all four query runs.

The graph shrinks from **438,672,627 to 249,794,087 bytes**. Ninja target-query peak
RSS falls from approximately **579 MiB to 398 MiB**, but query time rises from
**0.96–0.98 s to 1.10–1.15 s**. The experiment leaves commands and dependencies
intact. It supports a storage/memory improvement, not a speed claim; it does not
address Openmktr's much larger evaluation memory peak.

## Implementation order and acceptance

1. **Separate pattern indexing from memoization policy.** Keep indexing active.
   Add independent cache control and selective admission or a bounded fallback
   when recording and validation cost exceeds useful reuse. Prioritize repeated
   key/fingerprint allocation and cold-region churn. Safe is a useful existing
   profile control, but its full LibreOffice build still needs measurement.
2. **Reduce runtime context loading and restoration.** Use the real-context
   fixture as a regression benchmark. Compare CPU time and allocation as well as
   wall time; retain all variable, export, effect, cancellation and jobserver
   semantics. Do not remove checksums or make context inputs order-only to win
   a benchmark.
3. **Measure the remaining execution cost after those changes.** Trace native
   freshness/proof helpers, shell/exec overhead and the post-startup critical
   path. The historical clean run used 1,495 more CPU-seconds and averaged 28.65
   cores versus GNU's 31.19. Both historical clean runs had ample available RAM
   and zero swap. More allocated RAM alone is not the missing optimization.
4. **Qualify a fresh full pair at 32 jobs and equal RAM.** Require the established
   output, repeat-build, Writer/Calc/Impress and source-integrity checks, normal
   and sanitizer gates, and incremental correctness. Measure configuration,
   initial preparation, execution and complete invocation separately. Repeat
   with alternating order before claiming the 15.12% gap closed. Direct mode is
   also a useful control because it avoids Ninja emission and serialized runtime
   contexts, but its full LibreOffice performance is not established here.

The GNU-free Chimera `tests/bench.py` comparison passed with both matching
toolchain binaries. A disposable copy sets every job limit to 32, removes the
one/four-job lanes and uses the raw clock; the patch and all samples are retained.
Its small default-profile workloads show no material change in conversion or
recipe launch time, reinforcing that the LibreOffice result is workload-specific.
This investigation does not replace the production normal/sanitizer gates or CI.

## Retained exclusions and reproduction

The complete controller scripts live in the evidence archive and in
`/root/openmktr-speed-20261030` on this PC. They use fixed retained campaign paths
and fresh-directory guards; adapt output names before rerunning. Raw profiler
stacks, binaries, contexts and large graphs remain local and are not packaged.

The sampling run is separate from untraced timings. WSL's perf build lacks
ELF/DWARF unwinding support; leaf addresses were resolved with LLVM where
possible, and unresolved symbols remain marked. An execution probe after
diagnostic regeneration scheduled compiler work and was deliberately interrupted
after 35.36 s. It is not a no-op result or a complete clean result. Changed binary
paths and inherited environment can invalidate generated recipes; WSL_INTEROP
also changed across WSL sessions. No attribution of that probe to a production
correctness defect is made. Setup retries and both stopped controls are retained.
