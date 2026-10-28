# Local PC graph optimization evidence

`dependency-followup-2026-10-28.zip` is a separate follow-up snapshot. Its
`MANIFEST.json` checksums each entry; the outer
[follow-up report](../dependency-followup-results.json) records the ZIP checksum.
It preserves the original 115 ledger entries byte for byte and includes all
394 reservations under the user-authorized 400-command cap. No budget reset or
transfer of interrupted timings occurred. Original archives below are unchanged.

The final candidate is
`8027d04f6693a10801536d4e17da008b98595eb42875a652ee3e93eefbd240cf`.
Its local Chimera, sanitizer, Alpine/Gentoo, incremental and installed-overlay
checks pass. The historical Linux 6.12 Clang 20 variant passes its paired j8
clean comparison. The complete current Linux 7.3 matrix and default-safe direct
checks pass correctness and source integrity. Four no-op/source scenarios have
positive paired intervals; both clean intervals cross zero. Strict Linux
performance qualification and full profile promotion therefore remain
unresolved; safe and XZ remain defaults. Other current releases stay pending.

Earlier captures of 91 stopped containers remain intact. Late pre-update gate
containers are no longer present and their last raw logs were not captured.
Separate post-update reference, distribution, sanitizer, formatting and fallback
reruns all pass and supply fresh raw logs; missing logs were not reconstructed.

Raw profiler recordings remain in the local qualification volume with checksums
in the report. Compiler reproducer archives remain at their recorded local paths;
their root causes are not claimed fixed. Large release trees, binaries, profiler
recordings and compiler inputs are excluded from this ZIP. The source snapshot
matches the validated image; later documentation edits are stored separately.
The embedded report predates addition of its own ZIP checksum to the outer report.

The original archive and results below remain historical. The separate
`dependency-upgrade-2026-10-27.zip` contains the current dependency qualification:
115 of 200 large-command reservations, all failed stages, FFmpeg's one-warmup /
seven-pair samples, fresh Linux x86 correctness, suite results, installed package
inventories and current/frozen harness inputs. Its SHA-256 is
`8860e90a058458d875e50d8b612bc7197ed1b39850a5f2b2ac12d248354d4846`.
The embedded `manifest.json` verifies all 1,577 entries. It contains reports and
logs, not release source trees or compiled release binaries.

See [dependency upgrade results](../dependency-upgrade-results.json) for exact
binary/image identities, promotion criteria, all six failed FFmpeg intervals,
the incomplete Linux performance gate and Alpine workspace limits. Safe and XZ
remain defaults. Gentoo's installed-overlay, retained-workspace and six pinned
real-package checks pass; these historical package fixtures do not qualify the
other refreshed campaign releases. Hosted Actions were not dispatched and no
release was published.

`ninja-graph-pc-v4.zip` contains raw correctness results, benchmark command and
budget journals, timings, build logs, integrity checks, and retained failure
diagnostics. Its `manifest.json` records the byte length and SHA-256 of each
entry. Extract it into a disposable directory; paths reproduce the original
repository-relative evidence references without changing their bytes.

The historical candidate is `b7b75e459d4cf744ae73b970466c6c517473019cfce9573017770918c5eb147d`.
Its baseline is `192505c0257134de072b3d530c4a059ea12a33197b3cee20b8c219df26a58ce4`.
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
