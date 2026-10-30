# Openmktr remaining-gap implementation and comparison — 30 October 2026

Two fresh LibreOffice 25.2.7.2 pairs at **32 jobs and 30 GiB configured WSL RAM**
measure a **4.35% clean-build gap**, compared with the previously verified
**9.14%**. This closes **52.4% of the remaining
percentage gap** (4.79 percentage points).
The mean elapsed difference is **35.952 seconds**.

All four clean builds, twelve repeats, application smoke tests, original-source
audits, and cross-lane artifact controls passed. Two samples per tool show the
observed result on this PC; they do not establish a confidence interval or
universal Makefile compatibility.

## Complete clean-build comparison

| Pair and order | GNU Make (s) | Openmktr Aggressive (s) | Openmktr overhead |
| --- | ---: | ---: | ---: |
| pair1 (GNU first) | 814.768 | 857.170 | +5.20% |
| pair2 (Openmktr first) | 838.692 | 868.194 | +3.52% |
| Mean | 826.730 | 862.682 | +4.35% |

The prior means were GNU **845.172 s** and Openmktr
**922.413 s**, a **77.241 s** difference.
The new Openmktr mean is **59.731 s lower**
(6.48% of its previous total time).
Gap closure uses `(9.1390663 - new percentage overhead) / 9.1390663`, with
overhead defined as `100 * (Openmktr / GNU - 1)`. It is distinct from total-time
reduction. Historical and new absolute times also include machine/run variation.

The clean timer covers graph preparation and build execution. Configure/bootstrap
and configure-through-build totals are separate in the JSON. Each lane uses a
fresh, unmodified extraction, identical normalized configuration, Clang 18,
disabled ccache, 32 jobs and Ninja 1.13.2 where applicable. Caches were not flushed;
alternating order limits but cannot eliminate order effects. The project binaries
use matched Chimera Clang/LLVM 22.1.8, musl, libc++ and linker flags.

## Implemented optimizations

- Bound evaluation-recording admission inside expressions as well as statements.
  A rolling 65,536-lookup window rejects low-yield recording; a 128-MiB
  identity/fingerprint-work budget bounds large profitable-looking records.
  Active scopes finish with their certificates invalidated, preserving effects.
- Index runtime variable definitions using views into shared-lifetime buffers.
  Construct variable objects and recursive expression trees only when used.
  Preserve lookup, assignment precedence, introspection and target-local behavior.
- Validate large runtime contexts once per Linux launcher invocation and share
  immutable sealed memory with workers. Recheck file and executable identities;
  changed/unavailable contexts fall back to checksummed structural validation.
  Each recipe retains isolated evaluator state. Standalone helpers still work.
- Reuse lexical prerequisite normalization, escaped Ninja target names, and emitted
  normal-prerequisite text after exact dependency-sequence comparison. Preserve
  cycle filtering, duplicates' automatic-variable semantics, target scopes and
  order-only dependencies. Directory-alias sequences keep live filesystem checks.

The new regression tests cover a 70,000-iteration single expansion, nested
side effects, sealed-context reuse, changed/corrupt/unsealed contexts, protected
private descriptor transport, and shared dependency lists with target scopes.
The broader suite caught a generated manifest leaking into wildcard file lists;
its name now uses the existing hidden `.kati_` namespace. No new golden expectation
or known-crash exclusion was used to hide that failure. A further review found
that directory-alias filtering must observe filesystem effects between emitted
edges. The added regression fails on the pre-fix candidate and passes after
excluding directory-alias sequences from fragment sharing.

The measured dependency work is reused within a generation. Arbitrary Makefile
effects still require evaluation; this change does not claim complete graph-free
no-op execution. The investigation's conditional helper fusion and additional
path compression were not supported as speed priorities and are not credited as
implemented speedups.

## Focused controls

Generation-only controls use the same original source/configuration, with fresh
configured build directories or a private overlay of the populated tree. They
are separate observations, not complete build/no-op timings. The original graph
hash is unchanged.

| Generation control | Wall (s) | CPU (s) | Peak individual RSS (MiB) |
| --- | ---: | ---: | ---: |
| cold-base-1-generate | 65.013 | 65.455 | 6670.3 |
| cold-next-1-generate | 40.464 | 42.500 | 6611.0 |
| warm-base-1-generate | 56.493 | 54.315 | 9646.6 |
| warm-next-1-generate | 46.564 | 38.972 | 9337.4 |

The runtime fixture uses the same real 26.7-MB context, 256 output-producing
recipes, 32 workers, and six alternating batches per mode. All **6,144 outputs**
match. Launcher measurements include shared-snapshot preparation; standalone
measurements retain normal context validation. These isolated savings must not
be added mechanically to the clean-build savings.

| Runtime path | Median wall / 256 recipes (s) | Median child CPU (s) |
| --- | ---: | ---: |
| baseline-standalone | 0.777 | 22.120 |
| candidate-standalone | 0.540 | 15.080 |
| baseline-launcher | 0.809 | 23.190 |
| candidate-launcher | 0.150 | 2.480 |

The same-toolchain `tests/bench.py` comparison passed using a disposable copy with
all performance job limits set to 32. Its short per-operation guest-clock samples remain diagnostic in the JSON;
they are not used to quantify a speedup. The external 32-job GNU differential incremental
campaign and its harness tests also pass.

## Correctness and repeat builds

| Pair | Tool | Repeat 1 (s) | Repeat 2 (s) | Repeat 3 (s) |
| --- | --- | ---: | ---: | ---: |
| pair1 | GNU | 5.470 | 5.502 | 5.419 |
| pair1 | Openmktr | 88.616 | 65.981 | 64.874 |
| pair2 | GNU | 5.487 | 5.314 | 5.491 |
| pair2 | Openmktr | 96.916 | 66.053 | 64.962 |

All repeats preserve tracked installed payloads, compiler objects, archives and
ZIP outputs. Only the harness's existing permitted metadata timestamp refreshes
are allowed; blocking output changes are zero. Guarded edges can be visited
without rebuilding their outputs.

| Pair | Raw-identical installed entries | Identical ZIP payloads | Prefix-normalized scripts | Same-path ELF checks | Same-path objects / all objects |
| --- | ---: | ---: | ---: | ---: | ---: |
| pair1 | 3137 | 195 | 9 | 10 | 193 / 9507 |
| pair2 | 3137 | 195 | 9 | 10 | 193 / 9507 |

Installed entry sets, directory sets and permissions agree. Differing ELF/object
bytes contain build-path differences: isolated GNU rebuilds at the candidate's
exact paths match all of them byte for byte, and all selected ELF files were
relinked. The measured build trees remain unchanged. No differing entry is ignored.

Writer, Calc and Impress produce PDFs with the expected text in all four lanes;
dynamic-library checks pass. Every lane's **147,364 original source files and
symlinks** match the release archive, and candidate sources remain pristine after
the GNU controls. No third-party source was patched and no NAS was accessed.

Mean clean child CPU totals are **25774.6 CPU-s GNU** and
**26197.5 CPU-s Openmktr**. Average utilized cores, derived from
those means, are **31.18** and **30.37**.
Peak individual Openmktr repeat RSS is **8.832 GiB**. These totals do not
independently identify a remaining bottleneck.

## Validation, environment and evidence

All 38 local normal and ASan/UBSan gate commands pass: 181 correctness tests,
43 runtime-recipe tests, 58 graph tests, both 858-scenario snapshot profiles,
recursive execution, exports, regeneration, jobserver/cancellation, packaging
and related suites. No snapshot crash is quarantined. Formatting and the
incremental-campaign harness pass. Deliberate semantic limits such as `-j1`
remain in correctness tests; performance workloads all request 32 jobs.

Docker's Linux daemon is unavailable, so the Docker container gate was not run.
The retained local Chimera environment supplies the reference toolchain and
normal/sanitizer checks. An initial final-candidate incremental controller selected
system Ninja 1.11.1 and was rejected by the required-version check; its failed
evidence is retained, and the final campaign uses Ninja 1.13.2. The GitHub integration returned no workflow runs for
current HEAD; the uncommitted candidate has no remote CI qualification.

A first attempt was excluded after a guest-clock discontinuity during GNU
configuration; a second partial GNU build was excluded for the directory-alias
correctness fix. Neither contributes timing samples. Further clock probes found
that kernel tick/frequency parameters still changed after pausing the guest time
service, including when trying alternative clock sources and clearing pending
NTP corrections. The probes use the documented [adjtimex interface](https://www.man7.org/linux/man-pages/man2/clock_adjtime.2.html).

**All final clean/repeat and focused generation/runtime wall times above use
Windows Stopwatch**, captured independently at each flushed phase-start/end
marker. The final disposable harness retains guest raw-clock observations and
adjustment diagnostics without using them for speed ratios. Artifact and repeat
correctness checks are unchanged. The repository benchmark harness is unchanged.

The full controller measured **4299.851 guest raw
seconds** and **4300.096 Windows Stopwatch seconds**. These are
recorded as different clocks, not treated as interchangeable. Per-build host and
guest readings are retained in the JSON. Both tools had 30 GiB configured RAM
and 16 GiB swap; maximum sampled swap usage was 17,344 KiB. The original
24-GiB WSL configuration was restored byte for byte, the original clock source
was restored, and the time service is active again.

Candidate SHA-256: `3f6df564db782aabe2f749f360b05b140b8a4d31c61187d3eea4d678e21556e3`.

- [Machine-readable results](../validation/openmktr-performance-round2-results.json)
- [Evidence archive](../validation/evidence/openmktr-performance-round2-2026-10-30.zip)
- [Archive SHA-256](../validation/evidence/openmktr-performance-round2-2026-10-30.zip.sha256)
- [Prior 9.14% comparison](openmktr-performance-2026-10-30.md)
- [Optimization investigation](openmktr-next-optimizations-2026-10-30.md)

Raw trees, binaries and contexts remain on this PC under `/root/openmktr-round2-20261030`. The archive
contains selected logs, reproducible controllers, source hashes and the exact
patch relative to the prior candidate. Full runtime contexts and process
environments are excluded. Pre-existing workspace changes remain preserved.
