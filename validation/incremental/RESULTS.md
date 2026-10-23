# Incremental campaign — 2026-10-23

Status: **interruption recovery blocker resolved in the candidate**. The suite
and its release gate remain enabled, without an exemption for the original bug.
This remains fixture evidence, not a third-party compatibility claim.

## Candidate verification

The fix is based on updated `main` at
`949068e40940a9faa5adcd37d079e2c176580f9f`. Direct execution records catchable
build signals, forwards them to recipe process groups, waits for completion,
and removes regular outputs whose identity or precise mtime changed during the
interrupted recipe. Generated Ninja recipes use the same native output stamps
and signal cleanup. The launcher waits for Ninja before returning on interruption.
Ninja uses a synthetic recipe output for `.PRECIOUS` files so its own interruption
cleanup cannot delete them. Unchanged outputs are preserved. These handlers
cover HUP, INT, QUIT and TERM; they cannot execute after SIGKILL or power loss.

| Final check | Result |
| --- | --- |
| Differential campaign, `-j1` | 27/27 scenarios pass across GNU Make, direct Openmktr and Ninja |
| Differential campaign, `-j4` | 27/27 scenarios pass across all three engines |
| Rebuilt sets, clean oracle hashes, dependency probes, settled no-ops | No missed rebuilds, stale outputs, missing dependencies or unnecessary rebuilds |
| C++ units and revision generator | All pass |
| Behavioral correctness | 124 tests pass, including unchanged/new/precious output policies, parent-only termination, signal-failed ignored recipes, and independent Ninja scheduling |
| Compatibility regressions | 6 tests pass |
| Snapshot regression | 858/858 match; zero quarantined crashes |
| Include-dump smoke test | Pass |
| ASan/UBSan | C++ units, all 124 behavioral tests, and all 6 compatibility tests pass |
| GNU-free campaign self-tests | 5 tests pass |
| Changed C++ formatting | `clang-format-check` passes with the repository's Chromium configuration |

Fourteen snapshot expectations change only to reflect the generated cleanup
wrapper in printed Ninja commands. Their statuses and produced-file lists are
unchanged. No bug or crash is recorded as expected behavior.

The binaries were built and the portable checks run in the pinned Chimera chroot.
The differential campaign ran in WSL using the host GNU Make and Clang for fixture
recipes and a disposable copy of the musl candidate with its ELF interpreter path
adjusted to the private musl loader. Reports retain the exact executable digests,
tool versions, commands, source hashes and explicit environment.

The three-repeat `tests/bench.py` comparison uses baseline and candidate binaries
built with the same Chimera toolchain. Median conversion was 0.0584s candidate
versus 0.0596s baseline; no-op regeneration was 0.00078s versus 0.00085s. Flat
parallel execution was 0.205s for both; Ninja's 2,000 phony-recipe launch checks
were 0.615s versus 0.635s at `-j1`, and 0.176s versus 0.179s at `-j4`. These are
local measurements, not general speed guarantees. An actual non-phony Ninja
recipe now makes one native stamp query before it can modify an output; no-op
and phony recipes avoid this query.

Retained final local evidence is under
`validation/logs/incremental-fixed-2026-10-23/`, including both full campaign
reports, command records, recovery logs, portable/sanitizer logs and benchmark
JSON. Original complete campaign trees remain at `/root/incremental-final-j1`
and `/root/incremental-final-j4` in WSL. Docker's daemon was unavailable locally;
the new image/workflow executions require CI verification. Real third-party
campaign adapters and full mutation runs remain separate qualification work.

## Original reproduction

Initial status: **release blocked by interrupted-output reuse**. This is fixture
evidence; no third-party release matrix row is upgraded by these results.

Built the current C++ source at repository revision
`a7509ba5843d796523b1592b3d0f5461bd8a79d1` in the recorded pinned Chimera chroot,
using its Clang, musl, libc++ and Ninja toolchain. The version override was
`incremental-workspace`. Ran the external comparison in WSL with GNU Make,
host Clang for fixture recipes, Ninja and Python. To execute the musl Openmktr
outside the chroot, a disposable binary copy had only its ELF interpreter path
adjusted to a private musl loader/library path. Executable digests and tool
versions are retained in the reports. No runtime source was changed.

| Run | Scope | Result |
| --- | --- | --- |
| Serial, `-j1` | 3 build styles × 9 scenarios × 3 engines | 24 scenarios pass; 3 interruption scenarios fail for direct/Ninja |
| Parallel, `-j4` | 3 styles × 8 original scenarios × 3 engines | 21 scenarios pass; 3 interruption scenarios fail for direct/Ninja |
| Parallel discovery follow-up | New generated dependency, all 3 styles and engines | Pass |
| Final runner smoke | Depfile no-change scenario, all 3 engines | Pass |
| GNU-free harness self-tests in Chimera | 5 tests, including deliberately faulty engines and a shared missed dependency | Pass |
| Workflow YAML and whitespace review | Both workflows parse; `git diff --check` | Pass |

The full nine-scenario matrix is therefore covered at both concurrency levels.
No unnecessary rebuilding was reported. No baseline, clean-oracle or other
infrastructure error occurred in these completed comparisons.

The synchronized interruption damages `worker.o` before SIGINT. GNU Make removes
the interrupted output and recompiles it on recovery. Direct Openmktr preserves
the newer partial object and skips compilation. Ninja schedules the object edge,
but the generated timestamp guard skips its recipe. Both then attempt to link
the invalid object and fail. The clean oracle also identifies `worker.o` and
`app` as invalid/stale. The same failure occurs with explicit prerequisites,
compiler depfiles and recursive recipes, at both concurrency levels.

The CI/release checks initially failed on this finding. There is no
known-failure exemption; candidate remediation is verified above.

Retained local evidence is under
`validation/logs/incremental-2026-10-23/`: `summary.json`, per-run `report.json`
and `commands.json`, and interruption/recovery logs. Original complete trees
and logs remain in the WSL `/root/incremental-campaign-*` directories named
by those reports. These ignored local artifacts are not shipped as compatibility
claims. CI uploads independent evidence from the isolated comparison image.

During the initial suite-only run, Docker's daemon was unavailable and the full
portable/sanitizer suites were not rerun. The subsequent runtime fix and its
full Chimera checks are recorded above. The prior external build campaign still
requires real project adapters and full mutation runs before incremental
compatibility can be claimed for any of its releases.
