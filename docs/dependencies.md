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

External profile comparisons use explicit Safe/Aggressive selections, matched
toolchains, disabled compiler caches and cloned configurations. The completed
protocol uses one discarded warmup pair and seven alternating measured pairs.
Campaign runners persist command reservations in a locked ledger; failed and
interrupted commands remain accounted for. Historical allowances are recorded
in their reports and do not authorize new workloads. See
[performance results](ninja-performance.md) for completed comparisons and limits.

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

The [performance summary](ninja-performance.md) reconciles the initial
[dependency qualification](../validation/dependency-upgrade-results.json) and
[Linux/Alpine follow-up](../validation/dependency-followup-results.json).
The initial Alpine failures are resolved in the follow-up; their original
evidence remains available. Safe and XZ remain defaults. Historical
qualification does not establish support for untested release inputs.
