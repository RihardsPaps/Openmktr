# Dependency versions and upgrades

The supported reference build uses the stable packages in
[`tools/chimera-packages.lock`](../tools/chimera-packages.lock), including the base
system and transitive toolchain dependencies. The verified base image digest,
distribution repository, tested versions, compatibility image digests and CI
action commits are recorded in [`tools/dependencies.json`](../tools/dependencies.json).
The Chimera base digest was still current on 2026-10-27; the lock upgrades its
older system packages to the current repository versions.

The reference uses LLVM/Clang/LLD/libc++ 22.1.8, Ninja 1.13.2, Python 3.14.6 and
musl 1.2.6-r3. These are distribution-supported versions. Newer upstream LLVM or
Python releases do not cause an unsupported source bootstrap. C++17, O2 and
ThinLTO remain the project build settings.

The Docker build installs exact locked packages and compares the complete
installed set against the lock. If a rolling repository removes a version, the
build fails with a request to refresh the reviewed lock; it does not substitute
another version. The lock records versions, not a permanent package mirror.
Retain the tested image if later rebuilding that exact environment is required.
Refresh all installed packages together, including base packages, then review the
GNU dependency audit and run the reference suites before committing a new lock.

Both the checked-in build and generated graphs require Ninja 1.13.2, including its
FIFO jobserver support and interruption-status fixes. Direct mode does not launch
Ninja. Python provides the jobserver test's FIFOs; GNU Make is not a reference-test
dependency. Explicit Ninja job counts override its inherited GNU FIFO jobserver;
Openmktr's private recursive reservations and load admission remain separate.
The managed `omktr build` command now omits its automatic `-j` when a valid
inherited FIFO is present and no command-line `-j` was supplied. Explicit and
clustered job options retain priority. Missing, non-FIFO and ambiguous path
encodings keep the existing local job limit; dry runs retain their prior path.
Ninja controls its edges through the inherited pool. Recursive Openmktr admission
continues to use its separate private FIFO; the transports are never aliased.
Local FIFO readers are serialized to avoid waking every recipe for one token.
Releasing an inherited reservation wakes its waiters through a separate Linux
event descriptor. Notifications add no FIFO capacity. Unavailable event descriptors
retain bounded polling and cancellation checks; no extra runtime package is needed.

CI actions are pinned to verified stable release commits. Node 24 actions require
current hosted runners; a self-hosted runner must be at least 2.329.0. Artifact
uploads remain zipped and downloads extract them normally. The release script
uses the injected GitHub client and Node built-ins, compatible with github-script
9's ESM changes. Workflow publication still requires the existing release process.

Alpine and Gentoo lanes use pinned base digests and their own supported package
sets. They report their installed versions and are independently qualified; the
Chimera package lock is not imposed on another distribution. Current external
FFmpeg/Linux campaigns use `validation/ninja/Dockerfile.current`, based on the same
reference toolchain with external prerequisites. GNU packages in that image are
outside the GNU-free core. Historical issue reproducers and LLVM 19 images remain
available for reproducing their recorded evidence. New comparisons use unsuffixed
distribution LLVM tools; `benchmark_ninja.py --llvm-version N` explicitly selects
an older versioned toolchain.

[`validation/dependency-campaign.json`](../validation/dependency-campaign.json)
records current stable upstream campaign inputs separately from historical passes.
An input's archive hash does not establish compatibility. Unqualified releases stay
pending; current Meson-only Mesa is inapplicable to Make compatibility testing.
SDL 3 removed the historical Autotools entry point; any future CMake-generated
Makefile campaign needs its own defined scope.
`fetch_campaign_sources.py` verifies locked archives, while its explicit `--record`
option records locally observed hashes when establishing a new input. Hash
provenance distinguishes publisher metadata from local observations; neither is a
claim that an upstream signature was verified.

`qualify_dependencies.py` compares explicit safe/aggressive profiles using the same
binary and updated toolchain. Its required `--budget-ledger` holds a process lock
and persists/fsyncs every reservation before starting a large command. Use the
same ledger for further attempts. Failed commands and interrupted reservations
consume slots; the initial hard limit was 200. One shared configuration per project,
one discarded warmup and seven alternating measured pairs at j8/j16 consume 194
commands. Four direct-mode clean/repeat checks bring the matrix to 198. The two
remaining commands cover failed attempts; they are never silently replenished.
Core suites and small synthetic checks are separate. No monitor starts implicitly.
For the follow-up, the user explicitly raised the total cap to 250, then 335,
and finally 400 to complete Linux qualification after the fixes. Continuation tools accept
`--command-limit=400` and retain the same ledger and its original
reservations. The initial runner still defaults to the original 200-command cap.
An increased allowance does not relax the one-warmup, seven-pair acceptance gate.

Current Kbuild requires GNU sed's binary substitutions, so the external image
selects distribution `gsed` on a private PATH. Chimera has no GNU date package.
The campaign-only `reproducible-date` adapter implements Kbuild's fixed ISO
`-d`/`+%s` conversion with Python and delegates other calls to distribution date.
Both campaign runners check that conversion against `SOURCE_DATE_EPOCH` before
starting a large command. These prerequisites do not modify release sources or
enter the reference image.

The Linux follow-up uses `validation/ninja/Dockerfile.alpine` with supported
Clang/LLVM 22.1.3 packages and an explicit LLVM executable directory. The Chimera
22.1.8 comparison first hit an intermittent compiler SIGILL in unchanged kernel
sources; its saved standalone reproducer then succeeded. That failure remains
unresolved and is retained, rather than hidden by a successful retry. Both
Openmktr binaries still use the identical updated Chimera build toolchain and
flags; both compile Linux with the same Alpine compiler and environment. Only
this external comparison uses Dash for the historical baseline's shell timestamp
guards. The Alpine core lane retains BusyBox and tests the native timestamp fix.

The pinned Linux 6.12 reproducer predates Clang 21's
[const-initialization diagnostic](https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20250428/703580.html).
Its Clang 22 baseline fails with warnings treated as errors; that failed build
and its unchanged-source check are retained. A separate historical image uses
Alpine's supported Clang/LLVM/LLD 20.1.8 packages, an explicit executable directory,
and a fresh shared configuration. No release patches or additional warning
suppression are applied. The packaged compiler needs GCC startup/runtime files for host tools;
those packages stay in this external image, and compilation selects Clang/LLD.
Both Openmktr binaries still use the same current Chimera build toolchain.
This is a new comparison variant, separate from the old LLVM 19 execution-only
timings. Current Linux 7.3 qualification continues to use Clang 22.1.3.

The initial Alpine run exposed two workspace failures: distribution Python omits
`_zstd`, and BusyBox `test -nt` loses subsecond input changes. The follow-up uses
the supported distribution `zstd` executable when the Python extension is absent,
and generated freshness guards compare timestamps through Openmktr's native
helper. Mandatory workspace and graph suites now pass on Alpine and Gentoo.
The original failing evidence remains in the initial qualification archive.

The [2026-10-27 qualification report](../validation/dependency-upgrade-results.json)
and its checksummed evidence retain 115 of 200 large-command reservations.
That initial run passes selected FFmpeg 9.0.2 and Linux 7.3 x86 build correctness,
but all six FFmpeg performance intervals fail promotion and its Linux matrix is
incomplete. Zstandard's 37.9% larger archive fails the size
gate, so XZ stays the workspace default. The reference, required sanitizer,
release packaging and external incremental checks pass; Gentoo also passes the
installed-overlay and pinned real-package lifecycle checks. See the report for
the exact coverage and the original corrected test failure.
The separate [Linux/Alpine follow-up report](../validation/dependency-followup-results.json)
records subsequent fixes and measurements without replacing that initial report.
It completes the historical Linux 6.12 j8 clean comparison with a positive
paired interval using the supported Clang 20 variant. The complete Linux 7.3
j8/j16 matrix and default-safe direct checks pass correctness with unchanged
source contents. No-op builds improve by 29.16–32.87%, and single-source builds
by 7.69–7.98%, with positive paired bootstrap 95% intervals. Both clean-build
intervals cross zero, leaving the strict Linux improvement gate unresolved.
The ledger ends at 394 of 400 commands; six remain unused. Safe stays the graph
default, and the full twelve-scenario profile-promotion gate has not passed.
