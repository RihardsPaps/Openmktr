# Retained validation evidence

Current outcomes and comparison limits are summarized in
[build performance](../../docs/ninja-performance.md). This index preserves raw
reports independently of the documentation cleanup.

| Report or archive | Contents |
| --- | --- |
| [Completed core architecture](../../docs/core-refactor.md), [results](core-architecture-2026-10-31.json), [evidence](core-architecture-2026-10-31.zip), [SHA-256](core-architecture-2026-10-31.zip.sha256) | Invocation ownership, shared freshness, complete generation publication and bounded persistent resolution. Full cloud portable/sanitizer checks, 858/858 snapshots in both profiles, external incremental comparison, synthetic measurements and retained failed attempts. Real PC release qualification remains unrun. |
| [Core runtime refactor](../../docs/core-refactor.md), [results](core-runtime-refactor-2026-10-31.json), [evidence](core-runtime-refactor-2026-10-31.zip), [SHA-256](core-runtime-refactor-2026-10-31.zip.sha256) | First staged runtime delivery, pinned cloud Chimera normal/sanitizer checks, baseline failures, external incremental comparison and paired synthetic measurements. No-op scheduling improves; clean direct regression remains and real-project parity is unqualified. |
| [LibreOffice speed investigation](../../docs/openmktr-speed-investigation-2026-10-30.md), [results](../openmktr-speed-investigation-results.json), [evidence](openmktr-speed-investigation-2026-10-30.zip), [SHA-256](openmktr-speed-investigation-2026-10-30.zip.sha256) | Local 32-job, 30 GiB phase controls: disabling memoization while retaining pattern indexing reduces fresh graph preparation from 90.27 to 34.63 s. Safe/Aggressive controls, real-context restoration, graph compaction and Chimera benchmarks. Diagnostic prototypes only; no completed full-build speedup claim. |
| [LibreOffice improvements](../../docs/libreoffice-improvements-2026-10-30.md), [results](../libreoffice-improvements-results.json), [evidence](libreoffice-improvements-2026-10-30.zip), [SHA-256](libreoffice-improvements-2026-10-30.zip.sha256) | Fresh local 32-job GNU/Openmktr pair: 834.112/960.223 s; three verified repeats per tool, complete object/payload controls, clock controls, matched synthetic measurements and local Chimera/sanitizer qualification. All three performance objectives missed; container gate unavailable. Earlier failed attempts remain included and excluded from headline timings. |
| [LibreOffice runtime recipe results](../libreoffice-nested-recipe-results.json), [evidence archive](libreoffice-nested-recipes-2026-10-29.zip) | Nested generated reads fixed; local Chimera/sanitizer gates, capped component and installer builds, raw output comparisons and retained failed attempts. |
| [Full LibreOffice comparison](../../docs/libreoffice-full-comparison-2026-10-29.md), [results](../libreoffice-full-comparison-results.json), [evidence](libreoffice-full-comparison-2026-10-29.parts.md) | Verified local 32-job clean pair: GNU 821.39 s, fixed Aggressive 902.45 s. Wildcard blocker fixed; same-path GNU object/library controls pass. Repeat overhead, memory and graph-size defects documented. |
| [LibreOffice fixes](../libreoffice-fix-results.json) and [archive](libreoffice-fixes-2026-10-29.zip) | Variable lifetime and cycle-aware scheduling fixes, response-file correction, baseline failures, local normal/sanitizer gates and bounded SAL checks. Broader nested generated-file reads remain unresolved. |
| [LibreOffice investigation](../../docs/libreoffice-benchmark-2026-10-29.md), [results](../libreoffice-benchmark-results.json) and [archive](libreoffice-benchmark-2026-10-29.zip) | Local 32-job GNU build, reproducible Aggressive Ninja crashes, interrupted direct-mode underutilization, cycle diagnostics and checksummed raw evidence. No completed speed ratio. |
| [Dependency follow-up report](../dependency-followup-results.json) and [archive](dependency-followup-2026-10-28.zip) | Completed Linux 7.3 matrix, Linux 6.12 Clang 20 variant, distribution fixes, raw logs, integrity checks and retained failures. |
| [Dependency-upgrade report](../dependency-upgrade-results.json) and [archive](dependency-upgrade-2026-10-27.zip) | FFmpeg 9.0.2 Safe/Aggressive comparison, initial Linux attempts, installed packages and original failures. |
| [Graph report](../graph-optimization-results.json) and [archive](ninja-graph-pc-v4.zip) | Historical bounded campaign, evaluator/correctness evidence and interrupted compiler/runtime investigations. |
| [Ninja report](../ninja-performance-results.json) and [PR recheck](../pr-performance-results.json) | Earlier candidate measurements, workload definitions, identities and raw-log references. |
| [Compression report](../compression-results.json) | Mixed source/object XZ and Zstandard comparison. |
| [Issue 37 report](../issue37-results.json) | Historical execution-only comparisons, traces and compiler replays. |
| [Incremental results](../incremental/RESULTS.md) | Differential incremental correctness evidence. |
| [Portage qualification](../compatibility/portage-validation.md) | Recorded package lifecycle and workspace checks. |

Archive manifests checksum their entries; outer reports record archive hashes.
Extract into disposable directories and preserve original bytes. Historical
command budgets and reservations record completed campaigns, not permission to
start new ones. The final follow-up ledger retains 394 reservations under its
400-command cap, including failed and interrupted work.

The final measured Linux candidate has SHA-256
`8027d04f6693a10801536d4e17da008b98595eb42875a652ee3e93eefbd240cf`.
Its completed matrix passes correctness and source integrity, with positive
no-op/source intervals and inconclusive clean-build intervals. Safe and XZ
remain defaults. Subsequent source edits require their own qualification.

Earlier captures of 91 stopped containers remain intact. Late pre-update gate
containers and their last raw logs were unavailable; separate post-update reruns
supply fresh logs. Missing logs were not reconstructed. Large release trees,
binaries, profiler recordings and compiler inputs remain at checksummed local
paths where recorded and are excluded from the ZIPs. Compiler root causes are
not claimed fixed.

## Historical raw reports

The following reports retain unique measurements and probes from investigations
whose superseded narrative sections were removed:

- [cross-version-path-results.json](../../docs/cross-version-path-results.json)
- [cross-version-reuse-results.json](../../docs/cross-version-reuse-results.json)
- [recursive-cache-fixed-probes.json](../../docs/recursive-cache-fixed-probes.json)
- [recursive-cache-native-results.json](../../docs/recursive-cache-native-results.json)
- [recursive-cache-ninja-fixed-results.json](../../docs/recursive-cache-ninja-fixed-results.json)
- [recursive-cache-optimized-results.json](../../docs/recursive-cache-optimized-results.json)
- [recursive-cache-probes.json](../../docs/recursive-cache-probes.json)
- [recursive-cache-results.json](../../docs/recursive-cache-results.json)
- [recursive-cache-standard-bench.json](../../docs/recursive-cache-standard-bench.json)
- [ninja-clang-investigation-results.json](../ninja-clang-investigation-results.json)
- [ninja-performance-results.json](../ninja-performance-results.json)
- [pr-performance-results.json](../pr-performance-results.json)

## Implemented performance comparison — 30 October 2026

[openmktr-performance-2026-10-30.zip](openmktr-performance-2026-10-30.zip) and its [SHA-256](openmktr-performance-2026-10-30.zip.sha256)
retain two fresh 32-job GNU/Openmktr pairs, normal/sanitizer gates, incremental
checks, same-path artifact controls and controller scripts. The
[report](../../docs/openmktr-performance-2026-10-30.md) records a 9.14% remaining mean clean-build gap
and 39.6% closure of the historical percentage gap.

## Remaining-gap investigation — 30 October 2026

[openmktr-next-optimizations-2026-10-30.zip](openmktr-next-optimizations-2026-10-30.zip)
and its [SHA-256](openmktr-next-optimizations-2026-10-30.zip.sha256) retain
32-worker controls of evaluation admission, runtime context restoration and
recipe helpers, plus source-integrity and resource-restoration checks. The
[report](../../docs/openmktr-next-optimizations-2026-10-30.md) proposes the next
optimizations. Its generation-phase savings are not a new full-build comparison;
the verified 9.14% result above was authoritative at that stage. The remaining-gap implementation below supersedes it.

## Remaining-gap implementation — 30 October 2026

[openmktr-performance-round2-2026-10-30.zip](openmktr-performance-round2-2026-10-30.zip) and its [SHA-256](openmktr-performance-round2-2026-10-30.zip.sha256) retain
two fresh alternating 32-job GNU/Openmktr pairs, normal/sanitizer checks, focused
controls, output verification and controller scripts. The
[report](../../docs/openmktr-performance-round2-2026-10-30.md) measures a 4.35% clean-build gap and
52.4% closure of the previously remaining 9.14% gap.
