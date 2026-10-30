# Openmktr performance implementation and full comparison — 30 October 2026

Two fresh LibreOffice 25.2.7.2 pairs at **32 jobs and 30 GiB configured WSL RAM**
reduce the observed clean-build gap from **15.12% to 9.14%**. This closes
**39.6% of the historical percentage gap**.
Openmktr remains slower: the average difference is **77.241 seconds**.
All four builds, twelve repeats, application smoke checks, source audits, and
cross-lane artifact controls passed. These are two samples per tool, not a
statistical confidence interval or a claim of universal Makefile compatibility.

## Complete clean-build results

| Fresh pair and order | GNU Make (s) | Openmktr Aggressive (s) | Openmktr overhead |
| --- | ---: | ---: | ---: |
| pair1 (GNU first) | 836.852 | 910.889 | +8.85% |
| pair2 (Openmktr first) | 853.493 | 933.937 | +9.43% |
| Mean | 845.172 | 922.413 | +9.14% |

The historical pair was GNU **834.112 s**, Openmktr **960.223 s**. The new
Openmktr mean is **37.810 s (3.94%) lower**.
The normalized closure is `(old overhead - new overhead) / old overhead`, where
overhead is `Openmktr / GNU - 1`; it is not the percentage reduction in total build
time. Both new pair ratios improve on the historical ratio. Filesystem caches
were not flushed; alternating order limits, but does not eliminate, order effects.

Each clean timer covers the complete build invocation, including Openmktr graph
preparation and execution. Configuration/bootstrap are recorded separately in the
JSON, as are configure-through-build totals. All lanes use fresh unmodified
release extractions, identical normalized configuration, Clang 18, disabled
ccache, Ninja 1.13.2 where applicable, and 32 jobs. The project binary itself is
built with the same Chimera Clang/LLVM 22.1.8, musl and libc++ toolchain as v17.
No NAS was accessed. Semantic tests retain deliberate limits such as `-j1`;
all performance and release workloads use the maximum 32 logical CPUs.

## Implemented changes

- Pattern-variable indexing has its own `pattern_index` control, independent of
  evaluation memoization. Recursive option transport and cache identities include it.
- Evaluation stops admitting low-yield records after 65,536 expression misses
  when hits are below roughly five percent. This works inside large includes and
  nested evals: enclosing certificates are invalidated while active scopes finish.
- Runtime contexts are read directly into the payload buffer. Variable fields
  are decoded through string views, and recursive expression ASTs are constructed
  lazily after structural validation. Checksums, corrupt-input rejection, variable
  origins/flavors, introspection, exports and effects remain checked.
- Long plain absolute path prefixes are factored through Ninja variables under
  `compact`. Escaping remains unchanged for other paths.
- Fixed a discovered direct-executor race at `-j1`: dependency traversal and root
  dispatch now use the serial path. The regression failed all 48 variations with
  the old sanitized binary and passed with the fix. One snapshot now places each
  recipe's output immediately after its command announcement; no crash was hidden.

## Correctness and repeat builds

| Pair | Tool | Repeat 1 (s) | Repeat 2 (s) | Repeat 3 (s) |
| --- | --- | ---: | ---: | ---: |
| pair1 | GNU | 5.569 | 5.405 | 5.325 |
| pair1 | Openmktr | 108.600 | 83.432 | 81.379 |
| pair2 | GNU | 5.517 | 5.549 | 5.510 |
| pair2 | Openmktr | 123.053 | 84.462 | 81.857 |

All repeats preserve tracked installed files, objects, archives and ZIP outputs.
Only the existing permitted `setuprc`/`versionrc` metadata timestamp refreshes
occurred; contents, sizes and inodes remain unchanged. Guarded Ninja edges can be
visited without rebuilding their outputs. Repeat overhead is still much larger
than GNU's, despite improving on historical Openmktr repeats of 148.256, 110.122
and 109.958 seconds.

For **each pair**, installed entry sets, all 271 directory entries and permissions
match. Of 3,351 installed entries, 3,137 match directly, 195 ZIPs have identical
member payloads, and 9 debugger scripts match after lane-prefix normalization.
The remaining 10 ELF files and 193 of 9,507 compiler objects embed build paths.
An isolated mount-namespace control rebuilt them with GNU at the candidate's
exact paths: **all match byte for byte**, all ten ELF files were relinked, and the
measured candidate trees were unchanged. No differing entry was simply ignored.

Writer, Calc and Impress convert test inputs to PDFs with verified expected text
in all four lanes. Dynamic-library checks pass. Each lane's 147,364 original
source files/symlinks match the release archive; candidate sources also remain
pristine after the isolated GNU controls. The archive SHA-256 is
`21b38b7f429d305b229fb70f3294dd6ccd803f52dad0463d0923cdf143e9c8bd`.

## Focused measurements and remaining cost

The real-context fixture runs 256 recipes with 32 workers, four alternating
batches per binary, and verifies all 2,048 outputs. Median wall time falls from
**1.511 to 0.858 s**;
median child CPU time falls from **44.50 to 24.48 CPU-s**.
This is an isolated context-loading result, not a full-build speedup.

On the same final graph, compact paths use **301.8 MB**
versus **352.5 MB** with literal prefixes. Ninja emits
identical complete target listings (41,796 lines).
The compact query took 1.228 s with
664.3 MiB peak RSS; the expanded query took
0.873 s with 668.9 MiB.
This single observation shows a storage reduction, little RSS improvement and
slower graph loading; it is not credited as a speed improvement.

The same-toolchain `tests/bench.py` comparison passed using a disposable copy
with every performance job limit set to 32. It does not establish a broad speedup:
small synthetic conversion medians were 0.0982 s baseline and 0.1025 s candidate.
All samples are retained. Peak individual repeat RSS is **9.450 GiB**, down
from the historical 12.783 GiB, but still above the earlier 8-GiB objective.
The graph remains above 200 MB and clean execution remains slower than GNU.
Mean clean child CPU totals are 26386.8 CPU-s for GNU and 27128.5 CPU-s
for Openmktr; the remaining gap therefore includes extra CPU work as well as
less effective parallel use. These totals alone do not isolate the next bottleneck.

## Validation and evidence

The local Chimera normal and ASan/UBSan gates pass, including direct/Ninja
correctness, recursive execution, exports, regeneration, jobserver/cancellation,
42 runtime-recipe tests, 55 graph tests, and both 858-scenario snapshot profiles
with zero quarantined crashes. The new nested-admission, malformed-unused-expression,
context corruption, absolute-path and serial-order checks pass. Changed C++
formatting, the incremental-campaign harness and the external 32-job GNU
differential incremental campaign pass; the latter reports no release blockers
or performance defects.

Docker's Linux daemon is unavailable, so the Docker container gate was not run.
The GitHub integration returned no PR-triggered CI runs for current commit
`38ddf6d0e4c8bee9e5449992ed1c412205d6e56c`; no remote CI pass is claimed.

Timed runs temporarily paused guest time synchronization. Every measured phase
uses `CLOCK_MONOTONIC_RAW` and records realtime observations: **zero clock jumps**.
The entire controller measured 4583.834 s locally
versus 4584.094 s on Windows Stopwatch. Both lanes had the same
30-GiB allocation and 16-GiB swap configuration; peak observed swap usage was
0 KiB in pair 1 and 96 KiB in pair 2. Original WSL configuration and time-service
state were restored afterward.

Candidate binary SHA-256:
`af4435dfe3234f0a424e0de07b59219711e1551c943eeb2978de6969ad90e3a7`.

- [Machine-readable results](../validation/openmktr-performance-results.json)
- [Evidence archive](../validation/evidence/openmktr-performance-2026-10-30.zip)
- [Archive SHA-256](../validation/evidence/openmktr-performance-2026-10-30.zip.sha256)
- [Prior investigation](openmktr-speed-investigation-2026-10-30.md)

Raw release trees, binaries, graphs and runtime contexts remain on this PC under
`/root/openmktr-implemented-20261030`. The archive contains selected logs, results, controller scripts, source
hashes and the v17-to-candidate patch. It excludes full runtime contexts and raw
process environments. The working tree's pre-existing edits are preserved.
