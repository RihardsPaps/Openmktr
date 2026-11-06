# PR 74 Linux regression fixes — 6 November 2026

The Linux missing-object and following-graph regressions reported in
[PR 74](https://github.com/RihardsPaps/Openmktr/pull/74) are eliminated in
`2242f8fcbe42c4a3caf227ae89fd08ae0d497b28`. The optimizations remain enabled.
The [original report](pr73-followup-performance.md) retains the earlier failed
cohorts and their measured revisions.

## Changes

Fixdep wildcard normalization now advances through each record instead of
repeatedly shifting its remaining text. Bounded bulk reads replace per-line
stream extraction. The in-memory ownership index shares invocation-owned path
symbols; canonical dictionary IDs preserve deterministic serialization and
legacy v1/v2 decoding. Child decoding borrows immutable transport views,
validates skipped references and retains only its owner scope.

Versioned tree proofs distinguish ownership changes from removal or restoration
of an indexed regular object. Ninja rebuilds missing outputs without a root
graph refresh. Dependency-record deletion/content changes, unknown objects,
aliases and file-type changes still invalidate reuse. Frontend observations
remain independently validated.

The Linux observer uses bounded remote-memory reads with an aligned ptrace
fallback, including when bulk reads are denied. Argument-dependent BPF fast
paths retain caller-input checks before descriptor duplication. Exit polling
accepts handles for recorded descendants, including the race after their exit.
Literal compiler-probe validation still runs the compiler each time; exclusive
scratch creation and native cleanup remove utility processes. Unsupported shell
forms and PATH overrides for mkdir/rm retain the full script.

Two further Codex findings were fixed: shell continuations preserve token and
quote boundaries, and nested raw Ninja uses independent admission. Parent
leases remain held in their helper/evaluator while child launchers select their
own scope. Raw fallback scope includes the launcher PID at shell expansion
time, preserving stable Ninja command hashes. Regressions reproduce the wrong
Makefile import and deadlock before their fixes.

## Paired Linux 6.12 results

All workloads ran on the user's PC through Docker Desktop. No NAS was accessed.
The baseline is merged PR 73, `e466d484059e29f641142acd0fbcf81376b2ecc1`;
the candidate is `2242f8f`. Both use the same Chimera Clang 22.1.8, musl,
libc++, build flags and Ninja 1.13.2, with Aggressive,
`--use_ninja_phony_output` and eight jobs. Two warmup pairs precede seven
alternating measured pairs. No other local validation workload ran during
the timed cohort.

| Retained scenario | Baseline median | Candidate median | Median paired improvement, 95% interval |
| --- | ---: | ---: | ---: |
| Missing object | 12.709 s | 12.590 s | +1.03% [-1.19%, +1.62%] |
| Following Makefile check | 3.655 s | 3.458 s | +4.78% [+3.87%, +7.59%] |
| Source edit | 12.670 s | 12.626 s | +0.28% [-1.26%, +1.79%] |
| No-op | 3.389 s | 3.458 s | -2.28% [-7.53%, +4.21%] |

The earlier `a5e84b1` cohort recorded 22.958 seconds for missing objects and
13.857 seconds for the following graph check. The final comparison above
directly measures the candidate against merged PR 73. No final timing cell has
a statistically supported regression; the following-graph improvement is
supported. Missing-object/source/no-op intervals cross zero.

The strict combined timing/RSS gate remains **false**. The no-op point estimate
and interval miss its stronger thresholds, and small missing-object/no-op
peak-process RSS median increases miss its median-ratio criterion. The complete
results retain those failures. These data establish no no-op speedup or general
profile promotion. Safe remains the default.

Both arms retain the original verified clean trees and unpatched release
sources. Clean builds are not retimed. The retained harness restores source
mtimes and candidate fingerprint state between source-touch cases; following
graph checks follow object restoration. The archive retains the exact harness,
commands, per-run samples, binary/tool identities and bootstrap accounting.
The existing warning compatibility exceptions and reproducible timestamp
adapter are identical in both arms. External release prerequisites stay
separate from the GNU-free core.

## Correctness and review

Every measured sample passes configuration, source-integrity, artifact and
rebuild-set checks. No-op samples rebuild no objects; source/missing cases
rebuild `init/main.o` while leaving `kernel/fork.o` unchanged. A real
`init/do_mounts.h` touch rebuilds all three recorded owners, preserves the
unrelated object and retains both kernel artifact hashes:

- vmlinux: `8e95900686e9c4118f47bf5174cf65ea6f873ab8ff1b99406e62742890543b19`
- bzImage: `b300dfac67a2a8c2d7fea21b479dd2d0b5c797ab29413b6b5cf6ed4915f7b7a8`

The complete supported portable command sequence and complete CI sanitizer
sequence pass locally. Both 210-test correctness profiles and both 860-scenario
snapshot profiles pass, with zero quarantined crashes and no changed snapshots.
All 97 graph tests pass normally, under ASan/UBSan and in a freshly built Docker
image. Changed C++ formatting checks and the matched-toolchain standard
benchmark pass. Usual environment-specific skips remain recorded in the logs.
No kernel boot test was performed.

All Codex findings have been addressed. Codex's completed review of `2242f8f`
has no findings. The code revision passes the
[core CI](https://github.com/RihardsPaps/Openmktr/actions/runs/42257690958),
[external compatibility CI](https://github.com/RihardsPaps/Openmktr/actions/runs/42257691072)
and [release CI](https://github.com/RihardsPaps/Openmktr/actions/runs/42257691067).
The final documentation/evidence head is checked separately before merging.

The [machine-readable report](../validation/pr74-linux-regression-fix.json)
retains full timing and memory results, the standard benchmark and header check.
The [288-entry evidence archive](../validation/evidence/pr74-linux-regression-fix-2026-11-06.zip)
and [SHA-256](../validation/evidence/pr74-linux-regression-fix-2026-11-06.zip.sha256)
retain successful runs, intermediate failures and diagnostics. `manifest.json`
checksums every other entry. Temporary child-profile instrumentation is
identified separately, removed and rebuilt before qualification; its timings
are exploratory. Large binaries and release trees remain local.
