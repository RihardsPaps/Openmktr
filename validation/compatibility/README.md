# Optional compatibility environments

Run **Optional external compatibility and benchmarks** with Actions' manual
workflow dispatch on the branch to test. It supplements the required Chimera
correctness, sanitizer, and external incremental comparison gates. It does not
relax them or claim compatibility with every third-party Makefile.
Pull requests that change this workflow, its environment files, the jobserver
test, or the benchmark harness also run the lanes to validate those changes.

The pinned Alpine 3.24.3 lane builds with Clang, LLD, musl, libc++, and BusyBox. Alpine's
LLVM packages also pull in GNU toolchain components as distribution dependencies.
The Gentoo stage3 lane is an external glibc environment, with its own system
utilities and package build dependencies. Both are external comparison images;
neither changes the portable Dockerfile or Openmktr's runtime requirements.
Gentoo uses binary packages when available; source package fallback can make its
initial build slow. Base images are pinned; package feeds can move. Retain Docker build logs
and printed tool versions with results.
The Gentoo comparison image puts its reference Ninja first on its private PATH,
so installing Samurai does not substitute an implementation without FIFO support.
The installed Portage adapter continues to select `/usr/bin/ninja-reference`.

Both environments run unit, version, correctness, workspace, graph optimization,
Make compatibility, include dump, and explicit Ninja >= 1.13.2 jobserver checks.
Alpine installs its supported `zstd` package for Python builds without `_zstd`.
Workspace checks exercise subsecond input changes with a shell that rejects
`test -nt`, as well as both archive backends and interrupted publication.
The same jobserver suite is
now mandatory in the reference gate. Environment-specific snapshot
output is left to the reference Chimera gate. The jobserver check supplies a
GNU-protocol FIFO using Python, verifies a plain Ninja client, then runs generated
launchers with recursive Openmktr children, with and without explicit `-j1`.
It checks bounded concurrency, completion, failure propagation, and token return
under timeouts. GNU and Openmktr FIFOs are distinct; this does not claim that
Openmktr adopts an enclosing GNU Make job budget.

Ninja's [jobserver documentation](https://ninja-build.org/manual.html#_gnu_jobserver_support)
describes the no-`-j`, `MAKEFLAGS`, and FIFO conditions. A version below 1.13.2
fails the check rather than silently omitting coverage. Interruption tests verify
token return and successful recovery through the generated launcher.

The separate GNU reference job runs the existing differential incremental
fixtures at both `-j1` and `-j4`, using the
[external comparison image](../incremental/README.md). GNU Make is installed
only in that comparison image. Excessive work emits a warning and retains
evidence; stale outputs, missed work, and incomplete comparisons fail the job.

To run either toolchain locally from the repository root:

```sh
docker build -f validation/compatibility/Dockerfile.alpine -t openmktr-alpine .
docker run --rm openmktr-alpine
docker run --rm openmktr-alpine python3 tests/bench.py ./omktr > benchmark.json
```

Replace `alpine` with `gentoo` for the other environment. CI uploads build and
test logs, raw benchmark JSON for each environment, and GNU comparison evidence.
Timings are observations rather than pass/fail thresholds; compare baseline
and candidate on the same host and toolchain before drawing conclusions.

## Opt-in Portage integration

Changes to the Gentoo packaging, adapter, and load-limit paths also trigger
these lanes. Both environments run the portable `tests/portage.py` and
`tests/load_limits.py` suites. The Gentoo job additionally runs:

```sh
docker run --rm --cap-add SYS_ADMIN --cap-add NET_ADMIN --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
  -e OMKTR_PORTAGE_VALIDATION=1 \
  -e OMKTR_VALIDATION_IMAGE_ID="$(docker image inspect --format '{{.Id}}' openmktr-gentoo)" \
  -v "$PWD/portage-evidence:/portage-evidence" openmktr-gentoo \
  python3 validation/compatibility/portage.py --real-packages
```

Run this only in a disposable container: it temporarily configures Portage,
installs the live overlay ebuild from a local Git snapshot of the candidate,
including its `USE=test` suites, then installs and unmerges local EAPI 8 fixture
packages. A control package remains unselected. Direct and Ninja selections
cover configuration, compiler
flags, parallel builds, explicit `-j1`, load limits, unchanged outputs,
regeneration after an included configuration change, default test-target
probes (including fallback from absent `check` to `test`), tests, staged
installation, merge, unmerge, and intentional failures. During these checks
the system's `ninja` alternative points to Samurai; the installed Openmktr
must still use reference Ninja for its supported load-limit semantics.
The tool's installation must create no activation settings. The adapter's
portable checks cover argument quoting and signal propagation separately.

The installed helper also runs `portage_workspace.py`: three original fixture
releases check object reuse across identical versions, selective rebuilding
after a content change, normal Portage cleanup, and compression/restore between
versions. This covers the retained-workspace lifecycle, not arbitrary ebuilds.

The container permissions allow Portage to create its IPC, network, and PID
namespace sandboxes and bring up the isolated loopback interface. `SYS_PTRACE`
allows its sandbox tracer to follow maintenance that drops to the cache owner.
Portage
also needs AppArmor to permit mounting `/proc` inside its PID sandbox;
Docker's default profile on Ubuntu CI hosts blocks that mount.
The harness explicitly enables those features and fails if Portage reports that it cannot
start them. These permissions are only for this disposable
external test container; the portable test image does not need them.

Mount an **empty** output directory at `/portage-evidence` to retain command
logs, statuses and timings, tool/environment details, the installed binary
digest, phase evidence, and the final result. A nonempty directory is rejected
before changing Portage configuration; use a new directory for each retry.
The harness atomically records running, passed, or failed state and records
timeouts in the command ledger. Timed-out command groups are terminated.
The result also records the local candidate Git commit and tree identity.
Pass `OMKTR_VALIDATION_IMAGE_ID` as above to retain the exact Docker image ID;
without it the image field explicitly remains unknown. CI uploads evidence
even after failure.

`--real-packages` additionally qualifies the pinned Gentoo ebuilds for GNU
Hello 2.12.2, libpipeline 1.5.8, and gzip 1.14-r1, in both direct and Ninja
modes. It installs their dependencies, enables upstream tests, rebuilds from
source with Clang, merges the packages, and checks installed programs or a
linked library client. It retains the package database records, configured
Makefiles, compiler/configuration logs, upstream test reports with nonzero
test counts, and Ninja graphs. The libpipeline check requires evidence of
recursive Automake execution. Repository ebuilds and release sources are not
modified. These packages remain installed only inside the disposable container;
preexisting system packages are not unmerged. A focused rerun can select one
or more atoms, for example `--real-package '=dev-libs/libpipeline-1.5.8'`.
Omit both options to run only the local lifecycle fixtures.

Fixture success establishes this lifecycle coverage, not compatibility with
arbitrary Gentoo packages. This external image may use Portage's normal Bash
and GNU dependencies; the Chimera gate remains GNU-free.

[The local validation record](portage-validation.md) records the completed
Chimera and sandboxed Gentoo/Portage checks, environment details, and limits.

## Cross-version compiler-cache investigation

The separate opt-in [reuse campaign](../../docs/cross-version-reuse.md) compares
uncached, cold-cache and old-version-seeded builds of gzip and xz-utils, in
fresh Portage directories with direct and Ninja execution. It records phase
timings, compiler-cache inputs, output equivalence and upstream tests. This is
not part of the ordinary compatibility lane or a blanket package-cache safety
claim. Run `python tests/cross_version_cache.py` for the GNU-free evidence
accounting checks; ccache and Portage are needed only for the external campaign.

The bounded `cache_path_probe.py` follow-up replays three real xz objects
against old-version-only caches, with explicit positive controls and uncached
new-state comparisons. Its path profiles produced no cross-version hits;
the investigation stopped before the conditional C++ benchmark. See the
same report for the evidence, limitations and retained-container reproduction.

## Manager frontend and privilege checks

`python3 tests/manager_terminal.py` drives the real curses interface through a
private pseudoterminal. With Python's Tk support and Xvfb installed, start a
private display and run `DISPLAY=:99 python3 tests/manager_frontend.py` to
exercise actual Tk widgets on Linux. These checks use disposable configuration
and simulate a failed authentication at the frontend boundary.

`validation/compatibility/manager_polkit.py` separately exercises real pkexec
and polkit denial, authorization, and privileged stale-request rejection. Run
only in a disposable Gentoo container with `OMKTR_PORTAGE_VALIDATION=1`, an
installed `/usr/bin/omktr-manager`, a private system D-Bus/polkit daemon, and an
`omktr-manager-test` account. It temporarily authorizes only that account to
execute the installed helper, verifies root-owned output, and removes the rule
and restores configuration afterwards. It tests the privilege transition and
policy enforcement; it does not automate a desktop password prompt.

The installed manager fixture also parses optional GUI/TUI interpreter
dependencies using Portage, so invalid USE-dependency syntax fails the lane
even when the ebuild is installed with only the CLI enabled.
