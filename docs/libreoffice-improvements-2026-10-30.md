# LibreOffice fixes and verified local comparison, 30 October 2026

The final candidate completed the full configured LibreOffice 25.2.7.2 build and three verified repeat invocations. Its complete clean invocation took **960.223 seconds**, compared with **834.112 seconds** for GNU Make (+15.12% elapsed time). This is one observed comparison, not a statistically established speedup.

[Machine-readable results](../validation/libreoffice-improvements-results.json) include commands, full precision measurements, output manifests, qualification and controls. [Checksummed evidence](../validation/evidence/libreoffice-improvements-2026-10-30.zip) preserves the raw logs and implementation provenance. Earlier reports and evidence remain unchanged.

## Measured outcomes

| Measurement | GNU Make | Openmktr Aggressive |
| --- | ---: | ---: |
| Configuration | 10.292 s | 9.810 s |
| Bootstrap | 0.050 s | 0.063 s |
| Complete build invocation | 834.112 s | 960.223 s |
| Repeat 1 | 6.184 s | 148.256 s |
| Repeat 2 | 6.177 s | 110.122 s |
| Repeat 3 | 6.117 s | 109.958 s |
| Configuration through completed build | 844.457 s | 970.207 s |
| Build individual peak RSS | 0.728 GiB | 8.178 GiB |
| Largest repeat individual peak RSS | 0.224 GiB | 12.783 GiB |

The graph is **438,672,627 bytes**; all generated graph/runtime/script/state files together occupy **5,337,732,771 logical bytes** and **5,580,427,264 allocated bytes**. Offloaded sidecars are included in this total. The JSON splits graph statements, response contents, commands and every sidecar category.

Performance objectives: graph below 200 MB **MISS**; repeat individual RSS below 8 GiB **MISS**; faster than GNU **MISS**. These targets are not correctness gates. The historical report recorded a 753,053,731-byte graph and 19.342 GiB repeat individual RSS; the current measurements are lower, but that historical comparison uses different absolute lane paths and an earlier candidate. Retained sidecar storage remains large, so the graph-size reduction should not be read as a comparable reduction in total generated storage. Individual RSS is the wait4 process high-water mark; the separately recorded sampled tree RSS can count shared pages repeatedly and miss peaks between two-second samples.

## Implementation

Recipe identity now follows structurally inspected transitive variables, target scopes, exports and shell settings, with full-context fallback for computed or unbounded effects. Inspection traverses complete expressions without evaluating them. Runtime read certificates retain the actual successful variable observations, conservatively invalidate missing/corrupt state, and publish only after supervised recipe success. Successful recipe identities prevent timestamp freshness guards from hiding real recipe or relevant-variable changes. Unchanged sidecars retain timestamps; suitable guards use restat.

Include-remake dependency graphs have scoped ownership. Target-scope inheritance no longer leaks a map for each incoming edge. Scheduling edges are deduplicated while ordered and duplicate-sensitive automatic-variable lists remain intact. Exact shared prerequisite groups reduce repeated Ninja text without deleting compiler header dependencies. Large generated scripts use content-addressed sidecars and shared launcher prefixes. The diagnostic runs separately expose parsing, include remaking, dependency construction, recipe restoration and graph emission.

Bounded global and target variable records are decoded separately. Deferred evaluation uses the existing native supervisor and can exec the ordinary shell without a redundant intermediate fork/wait. Complex command cases retain the established process model. Cancellation, cleanup, exports, load limits and one jobserver reservation per recipe remain covered. There is no persistent service.

Deferred diagnostics use safely quoted POSIX printf, preserve multiline payloads, original filename/line, streams and error status, and avoid echo option/escape ambiguity. Filesystem observations include realpath, wildcard, shell/native-find and file functions, including nested/computed references. Added, changed and removed implicit exports invalidate deferred recipes; bounded contexts retain their exported definitions and computed contexts track the exported name set as well as successful reads. Recipe observations wait for prerequisites; parse-time evaluation retains its behavior. Runtime payloads are versioned as runtime-recipe-v5, so incompatible generated state is rejected.

Two repeat-specific defects were reproduced separately: include restarts inconsistently imported synthesized MFLAGS, and Ninja recreated deleted intermediate targets that GNU correctly skipped. MFLAGS is now normalized without overriding user definitions. Bounded intermediate proofs check consumer freshness and successful recipe identities; unknown or excessive cases do real work conservatively. Explicit intermediate requests and nested Ninja goals remain meaningful. Large consumer closures share bounded proofs without traversing order-only library subgraphs; the original scheduling edges remain present. Zero-input intermediates are skipped only with a valid successful recipe identity. Redundant consumer roots are removed, sharing one large closure within the 131,072-node / 64 MiB bound rather than duplicating its data. Current consumers also use those proofs when a context sidecar schedules them despite deliberately absent intermediates. Successful direct include-remake expansions capture actual reads in the original evaluator and commit them only after execution succeeds. This avoids relinking live bootstrap tools; failed work publishes no read certificate, and the original exports, assignments and job reservation remain intact. The benchmark runner accepts --repeats, defaulting to three. Existing user-facing build commands remain unchanged.

## Repeat and output verification

All six measured repeats returned zero. Compiler objects, static libraries and installed files were hashed with nanosecond timestamps, sizes and inode identities around each repeat. No compilation, linking, image archive rewriting or installed-content changes occurred. The only permitted metadata exception is LibreOffice rewriting setuprc/versionrc timestamps while preserving their bytes, sizes and inodes; exact per-repeat exceptions are in JSON.

Installed regular files, symlinks and directory sets/modes agree. Of the installed entries, 3,137 match directly, 195 match complete decompressed ZIP member maps (including duplicate-name occurrences), and 9 debugger scripts match after only documented lane-prefix replacement. The remaining 10 ELF files and 193 of 9,507 compiler objects match the isolated same-path GNU rebuild control byte for byte. Every differing ELF was actually relinked; all forced objects were regenerated. The measured candidate tree remained intact.

Both installations report version 25.2.7.2, resolve soffice.bin libraries, and start successfully to convert Writer, Calc and Impress inputs to PDFs containing the expected text. Both release-source trees pass the complete archive comparison; normalized configuration hashes match. No third-party source patches were made.

## Qualification and diagnostics

All 40 final local qualification commands passed, including portable contributor suites, C++ units, safe/aggressive snapshots, formatting, and the CI-defined ASan/UBSan suites. The focused suite has 40 tests covering direct execution and all three Ninja profiles, variable/automatic-variable semantics, exports, runtime assignments, generated includes, corrupt/missing sidecars, intermediates, failures, cancellation and parallel limits. Diagnostic payloads also pass under WSL dash. Fourteen changed wrapper snapshots were reviewed: exit statuses and produced files remain identical. Earlier wildcard semantics corrections remain preserved.

The final external incremental comparisons passed at jobs 1 and 4. An earlier four-job run returned exit 3 for one extra settled-noop relink after deleted-object recovery, while all content and dependency checks passed. Its recorded worker object timestamp was 0.630031878 seconds newer than the linked application; the cause is unresolved. Three isolated rechecks and a complete four-job rerun passed with the identical immutable binary. The original failure and all rechecks are retained. tests/bench.py passed with identically optimized Chimera baseline/candidate binaries. Separate dense shared-header memory scaling and runtime process-count fixtures retain raw results. Profiling and verification run outside timed LibreOffice invocations. The diagnostic-only instrumented binary is separately identified and never used for headline timings. Docker's Linux engine was unavailable: these are local Chimera results, not a completed container gate or fresh CI qualification.

## Diagnostic measurements

The separate graph-generation diagnostic records these scoped times. Nested scopes overlap, so the rows must not be added together. Its 0.1-second RSS samples retain log offsets; diagnostics use the restored WSL allocation and are excluded from the timed comparison.

| Phase | Recorded time, summed across restarts | Occurrences |
| --- | ---: | ---: |
| parse file time | 1.359967 s | 1 |
| parse eval time | 0.485457 s | 1 |
| eval time | 36.458402 s | 1 |
| remake included makefiles time | 1.175773 s | 1 |
| execute included makefiles time | 0.061509 s | 1 |
| make dep time | 9.549561 s | 1 |
| ninja gen (eval) | 16.370737 s | 1 |
| ninja gen (emit) | 5.916414 s | 1 |
| generate ninja time | 22.430897 s | 1 |

Matched dense shared-header fixtures use 512 headers and 16 variables per parent. These are synthetic scaling observations, separate from the full LibreOffice RSS measurements.

| Parents | Baseline peak RSS | Candidate peak RSS | Baseline / candidate graph bytes |
| --- | ---: | ---: | ---: |
| 128 | 83.96 MiB | 31.24 MiB | 829,992 / 63,294 |
| 256 | 160.70 MiB | 57.50 MiB | 1,647,400 / 107,676 |
| 512 | 325.95 MiB | 119.67 MiB | 3,282,216 / 196,220 |

The 64-recipe process fixture produced and validated every output. strace recorded 454 baseline versus 454 candidate clone/fork calls, and 461 versus 525 execve attempts. Counts include failed lookup/system-call attempts; they are not counts of unique successful programs. This fixture shows no net reduction in clone/fork calls and 64 more execve attempts, so it does not establish a reduction in total launcher overhead.

The separate diagnostic binary measured metadata reading, variable restoration and evaluation for a 6,000-variable fixture (10 samples each). Median times follow; full contexts deliberately retain conservative behavior for computed references.

| Context | Metadata reading | Variable restoration | Recipe evaluation |
| --- | ---: | ---: | ---: |
| bounded | 0.042 ms | 0.001 ms | 0.801 ms |
| full | 8.026 ms | 5.046 ms | 1.014 ms |

## Provenance and limits

All work ran on the user's PC in Ubuntu WSL's native filesystem, using 32 jobs and temporarily 28 GiB RAM / 16 GiB swap. The original WSL configuration was restored byte for byte. Guest systemd-timesyncd was temporarily stopped for the selected pair and restored to its original active/enabled state; Hyper-V synchronization and the clocksource were unchanged. Elapsed measurements use CLOCK_MONOTONIC_RAW, with an independent Windows Stopwatch reference. GNU ran first. Compiler caches were disabled; filesystem caches were uncontrolled. LibreOffice used the pinned archive and historical configure options with Ubuntu Clang 18. Openmktr baseline and candidate used matching Chimera Clang 22.1.8, musl, libc++, -O2 and ThinLTO flags. Their frozen source manifests and immutable binary hashes are in the evidence.

Candidate SHA-256: `e551c7fa3f7e6056d39ebdf864b64cdae1a8ec29d7ae81b3b4451694d0da1d59`. Baseline SHA-256: `1e1134c9113f2da4e35636db046d9b35b29c8e76a3e1c91964aa0f654152bc2f`. The source remains on the existing dirty branch; prior changes and historical evidence are preserved. Nothing was committed, pushed, merged or published as a PR.

The selected comparison uses a fresh GNU-first, Aggressive-second pair within one WSL session and the same resource allocation. There is no randomization, and filesystem caches remain uncontrolled. The earlier retained GNU baseline and all repaired-candidate attempts are excluded. Superseded lanes remain excluded: the initial candidate exposed a repeat graph syntax defect; v5 performed unwanted repeat compilation; v10 was stopped for a diagnostic-location fix; v11 was interrupted during its clean invocation; v11b completed its clean build in 897.824 s but its first repeat rewrote registries and image archives (78.21 s), so it failed verification; v13 failed near final linking when its superseded certification approach relinked a live bootstrap tool (903.44 s, exit 126); v15 completed its clean invocation in 935.31 s but its first repeat rewrote image archives (80.24 s), exposing the empty-intermediate and large shared-closure cases. The first fresh v17 pair completed GNU in 831.24 s and Openmktr in 951.88 s using the adjusted monotonic clock, but its first Openmktr repeat relinked three binaries and rewrote two gallery string files. Four prerequisite objects had newer timestamps than their consuming binaries despite recorded execution order. A subsequent 75-second clock control captured two backward realtime steps (about 0.77 and 0.81 s) and adjusted monotonic time running about 2.5% faster than raw time. Temporarily stopping guest NTP removed backward steps in the following 75-second control. Canonical documents the potential conflict between Ubuntu guest NTP and Hyper-V synchronization ([time synchronization guidance](https://ubuntu.com/wsl/docs/stable/explanation/time-sync/)). These observations explain why timing control was necessary, although they do not prove which adjustment affected each prior file. The selected pair uses the same immutable binary with guest NTP temporarily stopped and clock observations throughout. The independent Windows timer differs from either clean-build raw-clock measurement by less than 0.04 seconds; no timestamp tolerance was added to build semantics. Their logs are retained rather than pooled with the successful clock-controlled v17 lane. Failed setup/probe commands are likewise not presented as passes.
