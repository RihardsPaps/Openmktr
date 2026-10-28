# Build performance and qualification

Openmktr has measured improvements in incremental build overhead. Safe remains
the default graph profile: the completed comparisons do not establish a general
Aggressive-mode speedup or satisfy the full profile-promotion gate.

These are recorded measurements from 27–28 October 2026, not new tests of every
subsequent commit. Configuration, artifact and source-integrity checks establish
the scope of each result. Percentages are medians of paired improvements,
`100 * (1 - candidate / baseline)`; intervals are paired bootstrap 95% intervals.

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
