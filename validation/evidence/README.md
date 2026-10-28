# Retained validation evidence

Current outcomes and comparison limits are summarized in
[build performance](../../docs/ninja-performance.md). This index preserves raw
reports independently of the documentation cleanup.

| Report or archive | Contents |
| --- | --- |
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
