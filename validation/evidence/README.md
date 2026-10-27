# Local PC graph optimization evidence

`ninja-graph-pc-v4.zip` contains raw correctness results, benchmark command and
budget journals, timings, build logs, integrity checks, and retained failure
diagnostics. Its `manifest.json` records the byte length and SHA-256 of each
entry. Extract it into a disposable directory; paths reproduce the original
repository-relative evidence references without changing their bytes.

The current candidate is `b7b75e459d4cf744ae73b970466c6c517473019cfce9573017770918c5eb147d`.
The baseline is `192505c0257134de072b3d530c4a059ea12a33197b3cee20b8c219df26a58ce4`.
Both static binaries were built on the PC with the same pinned Clang 22.1.8,
musl/libc++, C++17, O2 and ThinLTO toolchain and flags. Release compilation uses
the same Clang 19.1.1, Ninja 1.11.1 and NASM runtime for both variants, in the
same network-free local Linux VM with native ext4 storage and 32 virtual CPUs.

The user capped the remaining campaign at 20 invocations. All 20 were consumed:
one cancelled control, two configurations, eight clean builds, eight incremental
invocations, and one diagnostic clean repeat. No more release builds ran after
the cap was reached. Earlier controls and interrupted campaigns remain separate.

The following percentage improvements are **single-pair observations**, with no
confidence interval or statistical acceptance claim:

| Project/jobs | Clean | No-op | Single source |
| --- | ---: | ---: | ---: |
| FFmpeg j8 | +7.34% | +70.53% | +79.78% |
| FFmpeg j16 | +1.71% | Not measured | Not measured |
| Linux j8 | -26.36% | Not measured | Not measured |
| Linux j16 | +0.98% | -0.49% | +5.03% |

Linux j8 took 283 seconds for the candidate versus 224 seconds for the baseline.
The final diagnostic candidate repeat took 266 seconds, still 18.86% slower
than that baseline. Both results are retained. The baseline and original
candidate have identical normalized build action counts (3,780) and output
object counts (3,006); the slowdown's cause remains unresolved. The diagnostic
repeat is separate and does not replace the slower measured pair.

All completed bounded builds pass artifact equivalence, configuration, required
rebuild, unrelated-output, source-integrity and applicable FFmpeg smoke checks.
The full Chimera portable and required sanitizer suites, formatting, 53 focused
tests, 576 evaluator comparisons and external incremental comparisons passed
before this reduced campaign. The new budget guard and evidence pairing tests
also pass without compiling releases.

The original seven-pair performance gates have **not** passed. Aggressive-mode
performance, feature ablations, separate Ninja runtimes and legacy performance
were excluded from the reduced campaign. Earlier WSL kernel panics and an
intermittent external Clang frontend crash are documented; passing retries and
the bounded campaign do not establish their root cause. The large preprocessed
compiler reproducer remains at its checksummed local path and is not bundled.
No binaries, VM disks, source archives or control-channel credentials are
included. See [`graph-optimization-results.json`](../graph-optimization-results.json)
for feature coverage, fallback boundaries and exact evidence identities.
