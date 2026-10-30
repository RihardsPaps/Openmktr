# Build performance and qualification

Openmktr has measured improvements in incremental build overhead. Safe remains
the default graph profile: the completed comparisons do not establish a general
Aggressive-mode speedup or satisfy the full profile-promotion gate.

The repeated Linux and FFmpeg comparisons below were recorded on 27–28 October
2026, not new tests of every subsequent commit. Configuration, artifact and
source-integrity checks establish the scope of each result. Percentages are
medians of paired improvements for those repeated comparisons,
`100 * (1 - candidate / baseline)`; intervals are paired bootstrap 95% intervals.

## LibreOffice 25.2.7.2 on the local PC

The [second implementation round](openmktr-performance-round2-2026-10-30.md) measures mean clean times of
**826.730 s GNU** and **862.682 s Openmktr** in two fresh alternating pairs at
32 jobs and 30 GiB RAM. The **4.35% gap** closes **52.4% of the
previously remaining 9.14% gap**, with full correctness and artifact controls.

The [implemented-performance follow-up](openmktr-performance-2026-10-30.md) runs two fresh pairs in
opposite orders at 32 jobs and 30 GiB RAM. Mean clean times are **845.172 s GNU**
and **922.413 s Openmktr**, a **9.14% gap**, closing **39.6%** of the
historical percentage gap. Both pairs pass full output controls, repeat checks,
application smoke tests and source audits. The following single pair is historical.

The [30 October verified follow-up](libreoffice-improvements-2026-10-30.md)
implements the reported correctness and memory changes. A fresh 32-job pair
completed in **834.112 seconds for GNU Make** and **960.223 seconds for
Openmktr Aggressive** (+15.12% elapsed time). Three repeats per tool passed
strict output audits; Openmktr took 148.256, 110.122 and 109.958 seconds.
Installed payloads and all 9,507 compiler objects agree after the documented
ZIP/prefix comparisons and isolated same-path GNU rebuild control.

The graph is 438.7 MB and repeat individual peak RSS is 12.783 GiB. The
below-200-MB, below-8-GiB and faster-than-GNU objectives all remain unmet.
This is one observed pair, with uncontrolled filesystem caches. Backward WSL
clock jumps were measured separately; the selected pair temporarily stopped
guest NTP and used raw elapsed time checked against Windows Stopwatch.
Original settings were restored. Local Chimera and sanitizer suites passed;
Docker's unavailable Linux engine leaves the container gate unrun.
[Results](../validation/libreoffice-improvements-results.json) and the
[evidence index](../validation/evidence/README.md) retain the full measurements,
synthetic comparisons, prior failures and qualification limits.

The [29 October investigation](libreoffice-benchmark-2026-10-29.md) used 32 jobs,
28 GiB WSL RAM and Clang 18 on the local i9-14900KF PC. GNU Make completed a
clean build in 828.23 seconds, averaging 30.66 CPU cores, and passed repeat,
PDF-conversion and original-source checks. Aggressive Ninja generation crashed
twice in fresh trees, including with an unlimited stack. A direct Aggressive
attempt averaged 1.27 cores before cancellation at the user's request. No
completed Openmktr build time or speed ratio was available in that initial
investigation. The detailed report
records generated-file probes, cycle warnings, debugger limits and raw evidence.

## Linux 7.3: Aggressive candidate versus historical binary

The [follow-up report](../validation/dependency-followup-results.json) records
one discarded warmup pair and seven alternating measured pairs per job count.
Baseline revision `58bb206a2e5a78f4ce0e7e01fce662f845ca459d` and the candidate
use identical updated Chimera compiler/linker flags. Both compile unmodified
Linux sources using Alpine Clang 22.1.3 in the same pinned comparison image.

| Scenario | j8 improvement | j16 improvement |
| --- | --- | --- |
| Clean | +0.31% [-0.58%, +1.05%] | +0.52% [-0.07%, +0.76%] |
| No-op | +29.16% [+27.47%, +39.00%] | +32.87% [+25.20%, +36.86%] |
| Single source | +7.98% [+5.54%, +8.19%] | +7.69% [+6.36%, +8.15%] |

The baseline uses the legacy profile; the candidate explicitly uses Aggressive.
These gains include implementation changes between binaries and do not isolate
Aggressive versus Safe within one binary. Baseline SHA-256 is
`7259555cbd7d10a3de7b320d6d165640ab42a1f6690693d359be485829ce5096`;
candidate SHA-256 is
`8027d04f6693a10801536d4e17da008b98595eb42875a652ee3e93eefbd240cf`.

Artifact, configuration, rebuild-set and source-content checks pass, along with
separate default-Safe direct clean/repeat checks. Both clean-build intervals
cross zero: they establish neither a supported regression nor the positive
improvement required by the strict six-scenario Linux gate. Four incremental
scenarios pass; the complete performance gate does not.

A separate historical Linux 6.12 j8 clean comparison with Clang 20 reports
+0.17% [+0.09%, +0.29%]. Its toolchain and workload differ from Linux 7.3,
so its samples remain separate.

## FFmpeg 9.0.2: Safe versus Aggressive

The [dependency-upgrade report](../validation/dependency-upgrade-results.json)
compares explicit Safe and Aggressive profiles of the same binary and updated
toolchain, with one discarded warmup pair and seven alternating measured pairs.
Negative improvements mean Aggressive took longer.

| Scenario | j8 improvement | j16 improvement |
| --- | --- | --- |
| Clean | -1.40% [-2.16%, -1.23%] | -2.03% [-2.41%, -1.99%] |
| No-op | -59.84% [-62.72%, -57.58%] | -68.49% [-69.56%, -67.14%] |
| Single source | -79.43% [-83.49%, -76.70%] | -80.41% [-86.97%, -78.80%] |

All six scenarios show supported regressions. Artifact, rebuild-set,
configuration, direct-repeat, smoke and source-integrity checks pass.
The twelve-scenario profile-promotion gate has not passed. Use the exact binary
identities in the report when interpreting this comparison; it predates the
final Linux candidate.

## Workspaces and compression

[Persistent workspaces](workspaces.md) retain source paths, objects and Ninja
state across prepared release directories. This is distinct from the opt-in
[recursive no-op cache](recursive-cache.md), which validates unchanged children,
and the closed [cross-version compiler-cache investigation](cross-version-reuse.md).

The [compression comparison](../validation/compression-results.json) found
Zstandard archives approximately 38% larger than XZ for the measured mixed
source/object workload. Zstandard packs and restores faster there, but exceeds
the 25% size threshold for selecting the default. XZ remains the default;
Zstandard is selectable. Ratios and cold-start costs depend on the payload.

## Evidence and reproduction

| Evidence | Scope |
| --- | --- |
| [Linux/Alpine follow-up](../validation/dependency-followup-results.json) | Final Linux matrix, identities, correctness checks, retained failures and exclusions. |
| [Dependency upgrade](../validation/dependency-upgrade-results.json) | Same-binary FFmpeg profile comparison and initial dependency qualification. |
| [Evidence archive index](../validation/evidence/README.md) | Checksummed raw logs, command journals and failure evidence. |
| [Earlier graph comparison](../validation/graph-optimization-results.json) | Historical FFmpeg 7.1.1 observations and incomplete kernel campaigns. |
| [Earlier Ninja comparison](../validation/ninja-performance-results.json) and [PR recheck](../validation/pr-performance-results.json) | Earlier binaries, synthetic overhead, FFmpeg 7.1.1 and workspace observations. |
| [Original fork comparison](https://github.com/RihardsPaps/Openmktr/pull/1) | Original C++ refactor validation and GNU dependency audit. |
| [Issue 37](../validation/issue37-results.json) | Historical execution-only Make/Ninja comparison; excludes configuration and graph generation. |

Earlier candidate timings, compiler failures and interrupted cohorts remain in
their original reports. They are not pooled into completed results. Unresolved
compiler failures are not claimed fixed. Historical passes do not qualify newer
project releases or later binaries.

For small overhead comparisons, follow [contributor benchmarks](../CONTRIBUTING.md#measure-performance).
For external releases, follow the [campaign guide](../validation/README.md).
Use matched toolchains, explicit profiles, sequential workloads and unchanged
release sources. The reference gate and optional compatibility environments
provide correctness coverage separately from performance qualification.
