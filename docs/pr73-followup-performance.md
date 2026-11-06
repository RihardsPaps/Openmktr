# PR 73 follow-up validation — 6 November 2026

The follow-up in [PR 74](https://github.com/RihardsPaps/Openmktr/pull/74)
implements fixdep ownership frontiers, bounded metadata batches, dynamic import
validation, missing-output ownership checks and parallel execution of computed
compiler recipes. Safe remains the default. These results do not qualify a
profile promotion or establish that every Aggressive workload is faster.

All local work ran on the user's PC through Docker Desktop. The core baseline
and candidate used the same Chimera Clang 22.1.8, musl and libc++ build flags,
with Ninja 1.13.2. No NAS was accessed. Third-party release sources were not
patched. External release prerequisites were installed in a separate comparison
container; they are not dependencies of the GNU-free core gate.

## FFmpeg 7.1.1

The baseline is merged PR 73, `e466d484059e29f641142acd0fbcf81376b2ecc1`.
The measured candidate is `a072b2332380f8a8c6329503745dafb82ef854f0`.
Both explicitly select Aggressive and `--use_ninja_phony_output`, with eight
jobs. Two warmup pairs precede seven alternating measured pairs. Configuration,
artifact hashes, source contents and rebuild sets match; every sample passes
the FFmpeg runtime smoke check. No-op samples rebuild no objects; source and
missing-object samples rebuild the selected owner without rebuilding the
unrelated object.

| Retained scenario | Baseline median | Candidate median | Median paired improvement, 95% interval |
| --- | ---: | ---: | ---: |
| No-op | 81.36 ms | 65.59 ms | 19.88% [18.46%, 20.69%] |
| Source edit | 357.59 ms | 332.89 ms | 6.17% [5.51%, 12.12%] |
| Missing object | 354.31 ms | 338.96 ms | 4.11% [3.87%, 6.59%] |
| Makefile timestamp | 81.64 ms | 64.50 ms | 19.19% [18.01%, 23.98%] |

The initial clean setup took 315.92 seconds for the baseline and 48.18 seconds
for the candidate. This is one diagnostic setup pair, not a repeated clean-build
qualification. The baseline serializes computed compiler commands as possible
recursive calls; the candidate serializes expansion, then releases ordinary
compilers to the shared eight-job budget. Separate regressions verify compiler
parallelism, serialized recursion and token availability while admission waits.

The strict combined no-regression gate is **false**: missing-object peak-process
RSS has a 188,200 KiB candidate median versus 187,680 KiB baseline, a 0.36%
increase. Its confidence interval crosses zero, but the gate requires a median
ratio no greater than one. All four wall-time cells pass their timing criteria.
Later changes address Kbuild frontiers and observer compatibility; these FFmpeg
timings remain attributed to the measured revision above.

## Linux 6.12

The measured retained candidate is
`a5e84b1b59648e1392904bbbbc9044dfdeea1323`, against the same merged baseline,
with the same explicit profile and eight jobs. The retry reuses verified clean
trees from the earlier experiment, whose clean candidate was `3d97db3`.
Clean setup timings are not remeasured or pooled with incremental samples.

Clang 22 introduces three diagnostics that Linux 6.12's configuration treats as
errors. Both arms use identical narrow `-Wno-error=` exceptions for
`default-const-init-var-unsafe`, `default-const-init-field-unsafe` and
`unterminated-string-initialization`. Both use the recorded ISO build timestamp,
the repository's unchanged reproducible-date adapter and external GNU sed 4.9.
The failed baseline attempts remain in the evidence. These controls qualify only
this compiler/configuration combination.

The initial candidate clean build produced matching artifacts, then its first
no-op exposed a dependency-index payload larger than the cache's 256 MiB bound.
The fixed representation stores paths once and owner references as 32-bit IDs;
the real 3,014-owner index occupies 16,456,165 bytes. Child frontends validate
the encoded index while retaining only their object-directory scope.

Earlier warmups also exposed unnecessary regeneration from covered source
metadata, refreshed source aliases, archive dependency-record timestamps and
leading `./` include aliases. Regressions and real-kernel probes verify their
fixes. Missing objects still require conservative ownership-proof refreshes.

The final paired results and the separate header-touch check are recorded in
the [machine-readable report](../validation/pr73-followup-results.json).

| Retained scenario | Baseline median | Candidate median | Median paired improvement, 95% interval |
| --- | ---: | ---: | ---: |
| No-op | 3.287 s | 3.584 s | -8.78% [-10.62%, +1.53%] |
| Source edit | 12.680 s | 12.660 s | +0.53% [-81.15%, +1.65%] |
| Missing object | 12.605 s | 22.958 s | -82.13% [-82.30%, -81.53%] |
| Following Makefile timestamp check | 3.678 s | 13.857 s | -276.55% [-278.80%, -273.51%] |

All seven pairs pass configuration, artifact, source-integrity and rebuild-set
checks. The overall performance gate is **false**. No-op and source intervals
cross zero; missing-object and following graph checks show supported regressions.
The latter follows object restoration and includes the ownership-proof refresh
from that state; it is not an isolated Makefile-touch measurement. These results
establish no general Linux speedup.

The later `c864232` adjustment avoids redundant timestamp-based discovery misses
for dynamic imports while retaining their sealed frontend validator and binding
reuse to the same immutable index. It also excludes private generated scripts
from in-tree source inventories. Its targeted kernel source/missing/graph/no-op
probe passes, but still shows the expensive ownership-proof path. The seven-pair
matrix above was not rerun at this later revision and is not relabeled as such.
The header-touch check at `c864232` rebuilds all three recorded `init/do_mounts.h`
owners, leaves `kernel/fork.o` unchanged and preserves both kernel artifact hashes.
No kernel boot test was performed.

## Correctness and review

The complete supported Docker gate and complete CI sanitizer command sequence
passed during implementation. At `a5e84b1`, 89 graph tests pass normally and under
ASan/UBSan, both 210-test correctness profiles pass, and both 860-scenario snapshot
profiles match without changed expectations or quarantined crashes. The later
`c864232` code passes 90 graph tests normally and under ASan/UBSan, plus the
210-test Aggressive correctness suite and all 860 Aggressive snapshots.
The admission fix at `ac1ce3f` passes 91 graph tests normally and under ASan/UBSan,
210 Aggressive correctness tests, all 860 Aggressive snapshots and 14 jobserver
tests. Its eight variable/substitution cases fail before the fix and pass in
native and standalone Ninja modes. A strengthened test proves actual runtime
deferral while literal compiler commands still reach three to four workers.
The C++ shared-header index round-trip and scoped-loading checks pass
normally and under sanitizers. Full local Gentoo compatibility passed after
fixing Bash/glibc startup observations; subsequent CI covers the final revision.

All six actionable Codex review findings have regressions and resolved threads:
duplicate absolute producers, two phony invalidation cases, consuming caller
stdin through descriptor transfers, changing its offset through seeks, and
releasing admission before shell-indirect recursive execution. The last fix
retains admission for dollar/backtick shell expressions; this can conservatively
serialize complex shell recipes. The performance tables predate this adjustment
and are not relabeled as its measurements. Codex's review at `ac1ce3f` reports
no further findings.

The [evidence archive](../validation/evidence/pr74-followup-2026-11-06.zip)
contains command journals, individual samples, metadata and tool identities,
retained failures, diagnostic controllers and local test logs. Its manifest
checksums every entry. The machine-readable report records the archive digest.
The [admission addendum](../validation/evidence/pr74-admission-addendum-2026-11-06.zip)
retains the final patch, source, failing-before evidence and passing validation.
