# Investigating Openmktr's remaining 9.14% gap

> Follow-up: the [completed implementation and comparison](openmktr-performance-round2-2026-10-30.md) measures a 4.35% remaining gap and 52.4% closure of the prior 9.14% gap. The investigation below is historical.

The next optimization should make evaluation-cache admission stop promptly inside
large expressions. With the current verified binary, bypassing evaluation
memoization while retaining pattern indexing reduces fresh LibreOffice graph
preparation from **60.330 to 31.703 seconds** across two runs per mode.
The **28.627-second phase saving** is equivalent to **37.1%** of the remaining
77.241-second clean-build gap. It is not a measured full-build improvement.

Runtime contexts are the second measured opportunity. Further graph-processing
work merits investigation, particularly for repeat builds. Basic process-launch
overhead is a lower priority on the evidence collected here.

The [completed comparison](openmktr-performance-2026-10-30.md) remains authoritative:
GNU **845.172 s**, Openmktr **922.413 s**, averaged over two fresh pairs.
No production source changes or new complete GNU/Openmktr pair were made in this
investigation. These optimizations are proposals.

[Structured results](../validation/openmktr-next-optimizations-results.json) and
[checksummed evidence](../validation/evidence/openmktr-next-optimizations-2026-10-30.zip)
retain individual samples, commands, controller scripts, statistics and checks.

## Conditions and scope

All workloads ran locally on the user's PC, using native WSL storage, **32 jobs
or workers**, **30 GiB configured RAM**, and 16 GiB swap. No NAS was accessed.
The binary is the previously verified Clang/musl/libc++ build, SHA-256
`af4435dfe3234f0a424e0de07b59219711e1551c943eeb2978de6969ad90e3a7`.
Project dependencies remain unchanged. The external LibreOffice configuration
and its Clang 18 toolchain match the retained comparison.

Preparation controls use `--ninja -j32` with statistics enabled; the disabled
variant adds `--disable_graph_optimization=evaluation`. The independent pattern
index remains enabled in every run. Fresh build directories receive the same
configuration and bootstrap. Populated-tree controls use private copy-on-write
mounts at the retained build's original path. The release source is mounted
read-only. These controls generate graphs and perform necessary included-makefile
work; they **do not execute the main Ninja application build**.

Timers use `CLOCK_MONOTONIC_RAW`. Time synchronization is paused during measured
controls and restored afterward. Filesystem caches are not flushed. Two fresh
pairs reverse order; populated-tree samples show more variation. One populated
disabled sample overlapped a read-only inventory and is excluded, with a separate
replacement retained. This small sample is not a confidence interval.

## 1. Bound cache admission at expression boundaries

| Generation-only control | Current adaptive cache (s) | Evaluation cache disabled (s) |
| --- | ---: | ---: |
| Fresh pair 1 | 61.808 | 31.741 |
| Fresh pair 2, reversed order | 58.852 | 31.665 |
| Fresh mean | **60.330** | **31.703** |
| Populated sample 1 | 58.458 | 31.573 |
| Populated sample 2 / replacement disabled sample | 48.385 | 35.063 |
| Populated mean | **53.421** | **33.318** |

The fresh controls each perform one C compilation, one link, and 303 dependency
actions while remaking includes. Their graph statistics agree: 24,714 nodes,
21,448 recipes, 33,169 commands and 187,330 dependencies. Normalized configuration
hashes and target names/order match. Populated target-and-rule listings also
match. Eight fresh rule assignments differ across the different-length build
paths: scripts cross the 4,096-byte sidecar threshold, which also affects restat
selection. These fresh graphs are not claimed byte-identical or fully validated
application builds.

The current cutoff in [BeginStatement](../src/incremental_eval.cc) tests whether
misses exceed 65,536 and hits are below approximately five percent of misses.
The fresh logs show it actually firing after **3.30–3.34 million misses** and
only 38,553–52,149 hits. `BeginRegion` increments misses during expression
evaluation, but admission is reconsidered only when the next statement begins.
Large expansions therefore overshoot the intended budget dramatically.

`postprocess/CustomTarget_images.mk` contains a nested `foreach` over the icon
inventory. Its inclusive time is roughly 7.2–7.7 seconds with recording versus
roughly one second without it. This and the source control flow support the
large-expression diagnosis; there is no per-expression sampling trace here.
The aggregate first/second fresh evaluations fall from 15–18 seconds each to
about 2.6 seconds each. Include execution remains about 13 seconds and final
Ninja evaluation/emission remains about eight seconds.

**Proposed implementation:** check admission before starting expression regions,
using a bounded observation/identity-byte budget as well as a rolling hit-rate
window. Stop key construction, fingerprinting and recording when the budget is
exhausted. Invalidate active enclosing certificates while preserving scope stacks,
operand numbering and normal evaluation. The existing `Read` path can still
fingerprint a binding before discovering that all active regions are unusable;
avoid that work once recording has been disabled. Keep pattern indexing
independent and preserve caching for workloads where reuse pays for its cost.

**Acceptance:** add a large single-statement nested-expression regression that
checks bounded admission, alongside nested eval, exports, computed references,
side effects and replay correctness. Run direct and Ninja suites, sanitizers and
the existing reusable-cache fixtures. Compare with the same binary/toolchain and
fresh full builds. Aim to approach the disabled-cache preparation control without
making the disabled setting the universal default.

If all 28.627 seconds carried through unchanged, Openmktr would take about
**893.786 s** and the remaining gap would be **5.75%** against the existing GNU
mean. This is conditional arithmetic; new complete builds must establish it.

## 2. Make runtime variable contexts lazy beyond their ASTs

The fresh graph references **4,356** deferred recipe sidecars. Of these,
**3,454** reference a 26.67 MB global context. Counting each recipe once gives
**92.20 GB** of logical context input across all references. This is not measured
physical disk traffic; the page cache can serve the data.

The runtime experiment uses the current populated build's 26.70 MB context with
4,574 definitions: 1,678 simple and 2,896 recursive. Six alternating batches per
variant run 256 trivial recipes with 32 workers. All 3,072 outputs are verified.

| Context, current binary | Median wall time / 256 recipes | Median child CPU time |
| --- | ---: | ---: |
| Small fixture | 0.101 s | 0.48 CPU-s |
| Real LibreOffice context | 0.722 s | 20.55 CPU-s |

This fixture differs from the earlier 32.74 MB context experiment; the changed
absolute timing is not a new code improvement. It omits production supervisor
state publication and deliberately gives both variants the same tiny recipe.

[RunRuntimeRecipe](../src/command.cc) still reads and checksums the entire context,
iterates every definition, constructs all variable objects, and interns original
recursive text. [RestoreLazyRecipeExpression](../src/expr.cc) validates every
serialized AST and owns a copy of its record before deferring AST construction.
The first round of lazy AST work therefore leaves substantial per-process work.

**Proposed implementation:** first index definitions in a shared-lifetime buffer,
use views for immutable text, and materialize variable objects on lookup. Then
prototype a build-owned, immutable validated context snapshot so workers can
reuse validation. A single-thread context broker that forks isolated children
is one Linux design to measure; standalone helpers retain the existing fallback.
Read-only mapping alone does not remove checksum or structural-validation cost,
and an ordinary mutable file must not be trusted merely because it was validated
earlier. Do not introduce a persistent mutable evaluator shared across recipes.

**Acceptance:** preserve computed references, target-local overrides, environment
exports, `origin`/`flavor`/`value`, symbol enumeration, eval mutations, corruption
rejection (including unused malformed records), cancellation, successful-read
publication and recipe isolation. Measure checksumming, validation, allocation
and evaluation separately before choosing the shared-context mechanism.

Scaling the fixture's 20.07 CPU-second difference by 3,454 / 256 gives about
**271 CPU-seconds**, or **8.46 seconds at ideal 32-core utilization**. This is an
optimistic scale estimate if that overhead were eliminated, not a wall-time
prediction or a saving to add mechanically to the preparation result.

## 3. Reduce repeated dependency and graph processing

With recording disabled, populated-tree preparation still takes **31.6–35.1 s**.
Its final dependency resolution takes **8.1–8.9 s** and Ninja evaluation/emission
takes **15.0–17.3 s**. The populated graph has **2,823,734 dependency references**,
versus 187,330 in the fresh control, and 41,409 nodes. Peak individual RSS remains
about nine GiB even without evaluation recording.

**Proposed implementation:** profile allocation/copying and filesystem observation
within these phases; reuse immutable prerequisite sequences and validated include
parses, and preserve generated graph sections when their inputs and effects are
proven unchanged. Scope stat reuse to a phase and invalidate it across recipe or
shell effects. Avoid rebuilding the complete graph merely to learn that most
outputs are current. Preserve generated includes, depfiles, environment and
wildcard changes, deleted intermediates and arbitrary Makefile effects.

This primarily targets the previously measured 81–123-second Openmktr repeats.
The populated-generation control is not itself a complete no-op timing, and its
phase costs must not be attributed wholesale to the clean-build gap. Capture a
full phase/critical-path trace after the first two changes to choose the next
clean-build optimization on evidence.

## Lower priorities

A separate 32-worker fixture uses six alternating batches of 512 output-producing
recipes per variant, verifying all 9,216 outputs:

| Path | Median wall time | Median child CPU time |
| --- | ---: | ---: |
| Shell directly | 0.162 s | 0.160 CPU-s |
| Recipe supervisor plus shell | 0.174 s | 0.575 CPU-s |
| Supervisor, native freshness check, shell | 0.186 s | 0.820 CPU-s |

Basic helper startup is much smaller than full-context work here. This fixture
does not model successful-state certificates, FIFO contention or large environment
scripts, so it does not exonerate all wrapper costs. If production profiling
identifies them later, fuse native freshness checking and runtime evaluation into
the existing supervisor while retaining job budgets, cleanup and cancellation.

Further path compression is also a low speed priority. The previous verified
control reduced graph size but made the single Ninja parsing query slower
(1.228 versus 0.873 seconds). It supports a storage benefit, not this speed goal.
Additional threads are not the answer: these workloads already request all 32.

## Interpretation and checks

The completed pairs average 31.22 utilized cores for GNU versus 29.41 for Openmktr,
with Openmktr using 741.71 additional CPU-seconds. Dividing that CPU difference by
32 accounts arithmetically for 23.18 seconds of the 77.24-second elapsed gap; the
remaining 54.06 seconds is the difference in unused 32-core capacity. This is an
accounting identity, not a causal split. Serial preparation, dependency waits,
I/O and the critical path can all contribute.

All nine generation commands completed; one timing is excluded as described
above. All runtime/helper outputs pass. The retained original graph's SHA-256
is unchanged. A fresh audit checks **147,364 original release files/symlinks with
zero differences**. No third-party sources were patched. Full correctness and
sanitizer suites were not rerun because this pass changed no production code;
their prior results remain in the completed comparison. Each proposed production
change requires new gates and alternating full GNU/Openmktr pairs.

Temporary WSL memory settings and time synchronization were restored after the
investigation; the evidence records the restoration checks.
