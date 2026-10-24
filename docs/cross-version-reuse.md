<!-- Copyright 2026 Rihards Paps and Haralds Paps; SPDX-License-Identifier: MPL-2.0 -->

# Cross-version compiler-cache investigation

Portage uses `${PORTAGE_TMPDIR}/portage/${CATEGORY}/${PF}/work`, where `PF`
includes the package version and revision. An upgrade therefore starts with a
different build directory. Openmktr's `--recursive_cache` validates a no-op in
an existing directory; it does not restore objects into the new directory.
See the [Gentoo variable reference](https://devmanual.gentoo.org/eclass-reference/ebuild/)
and [recursive-cache contract](recursive-cache.md).

This opt-in external campaign tests whether an existing compiler cache can
reuse compilation results across that boundary. It adds no Openmktr runtime
option or artifact format. The GNU-free portable gate checks the evidence
accounting without installing ccache or Portage.

## Recorded result — 2026-10-24

The tested conservative ccache configuration produced correct outputs but
increased median full-build time by **4.3% to 12.5%**. These two small package
upgrades do not justify implementing a custom artifact cache. The bounded
path-handling follow-up below also failed to qualify cross-version object
reuse. **Stop this investigation at that gate**; a compilation-heavy C++
upgrade was conditional on successful xz reuse and was not run.

All **102 controlled combinations** and **120 measured new-version builds**
passed. Each of the eight package/mode/path combinations has five samples
for each cache state. All comparisons were independently rechecked from the
completed result: object and executable/library hashes, emitted dependencies,
compilation diagnostics, staged ELF artifacts, upstream test reports and
runtime probes. Gzip ran 34 upstream tests per build; xz-utils ran 22, with a
client linked against staged liblzma. Source archives and repository input
manifests retained their recorded hashes.

| Upgrade | Executor | Paths | Uncached median | Old-version-seeded median | Change |
| --- | --- | --- | ---: | ---: | ---: |
| gzip 1.14-r1 → 1.15 | Direct | Original | 31.32 s | 34.28 s | +9.5% |
| gzip 1.14-r1 → 1.15 | Direct | Normalized | 31.16 s | 33.83 s | +8.6% |
| gzip 1.14-r1 → 1.15 | Ninja | Original | 31.15 s | 34.19 s | +9.8% |
| gzip 1.14-r1 → 1.15 | Ninja | Normalized | 30.98 s | 33.78 s | +9.0% |
| xz-utils 5.8.3 → 5.8.4 | Direct | Original | 25.46 s | 28.64 s | +12.5% |
| xz-utils 5.8.3 → 5.8.4 | Direct | Normalized | 25.82 s | 26.93 s | +4.3% |
| xz-utils 5.8.3 → 5.8.4 | Ninja | Original | 25.62 s | 28.68 s | +11.9% |
| xz-utils 5.8.3 → 5.8.4 | Ninja | Normalized | 25.71 s | 26.97 s | +4.9% |

The machine was an i9-14900KF, with 32 logical CPUs available to Docker on
WSL2, four build jobs and no container CPU quota. The experiment used Clang
22.1.8, ccache 4.14, reference Ninja 1.13.2 and the Gentoo snapshot dated
`2026-10-24 00:45 UTC`. Runtime sources matched `6d1b49e`; the selected
Openmktr binary's SHA-256 was
`6a0877d5706b4e4c22870617454042f8de3dd17bae325c419bd373c9edde9cc3`.
The prepared image ID was
`sha256:9661da08a219badeeeea9f3f26bd095f497b808996152df7e037b4cf540f0177`.

The [structured results](cross-version-reuse-results.json) retain every sample,
phase duration, CPU measurement, cache-hit count, cache size, output count,
sample-manifest digest and source identity. Full local evidence is under
`validation/logs/cross-version-20261024/qualified/`, including compiler journals,
test reports, the measurement tooling snapshot and untimed cache-input diffs.
Earlier `*-dev*`, `probe*` and interrupted `final/` directories are bring-up
evidence and are excluded from the reported measurements.

### Why the hit rate is not an upgrade speedup

- Gzip had **zero compile-phase hits** in every cold and seeded sample.
  Normalized seeded builds had nine configure-probe hits, compared with one
  in cold builds. The diagnostic replay of `bits.o` also showed real
  preprocessed input changes, including gnulib header changes; those misses
  cannot safely be discarded as path noise.
- Xz-utils had only one to three compile-phase hits in cold builds and two
  to three in seeded builds. Duplicate work within one build can therefore
  account for much of this small count; it is not proof of useful upgrade
  reuse. Normalized seeded builds had 94 configure-probe hits versus four
  cold, but their compile phase still took roughly 7.2 seconds compared
  with 6.1 seconds uncached.
- The xz-utils replay of `alone_decoder.o` retained absolute versioned source
  and include paths in ccache's preprocessor input, despite Clang's prefix
  maps. This is a concrete remaining path constraint. The debug-path fixture
  likewise restored two relative-source objects but missed the object
  compiled from an absolute source path.
- The real package configurations emitted no depfiles. Their 51 gzip and
  130 xz-utils object hashes were compared, but post-hit incremental
  dependency coverage comes from the controlled fixtures. No broader real
  package incremental guarantee follows from these fresh builds.

These initial results motivated a bounded qualification of ccache's directory
handling on absolute-path builds, recorded below. A custom layer would need
evidence of reusable work still unavailable through existing tools, and
complete input tracking for that work. These results supply no such
justification. Expensive C++ builds and other package upgrades may have
different results; neither their performance nor their safety is claimed.

### Bounded path follow-up — stop decision

The follow-up replayed three corresponding real liblzma compilations from
the retained xz 5.8.3 and 5.8.4 configured trees: `alone_decoder`,
`vli_decoder` and `crc32_fast`. It used ccache 4.14 and the original pinned
Clang/environment. Release sources, generated configuration and ebuilds
were not edited. This was an untimed object feasibility probe, not another
package performance measurement.

Each profile tested all three objects both without dependency generation
and with explicit `-MD -MF` generation, for **18 real-upgrade comparisons**.
A second set of **18 relocation-only controls** copied identical old content
into both versioned layouts. This separates path constraints from actual
changes in the new release. The results were the same in both workloads:

| Profile, retaining the original Clang prefix maps | Cross-version hits | Exact object matches | Dependency coverage matches |
| --- | ---: | ---: | ---: |
| `CCACHE_BASEDIR` unset | 0 / 6 | 6 / 6 | 3 / 3 |
| Common parent `/var/tmp/portage/app-arch` | 0 / 6 | 0 / 6 | 3 / 3 |
| Each version's own `work` directory | 0 / 6 | 0 / 6 | 3 / 3 |

For each comparison, the probe compiled the new state uncached, populated a
fresh private cache with that old-version compilation only, verified an
identical same-directory replay hit, restored the old-only cache, then
compiled the new state through ccache. **All 18 same-directory positive
controls hit and matched their seed objects, diagnostics and dependencies.**
Another 18 same-directory controls passed in the relocation-only workload.
Both experiments completed successfully, but no profile qualified for the
next stage: a passing experiment status is not a passing cache configuration.

Ccache's relative rewriting left the versioned source-directory component
in the compiler input, for example:

```text
../../../xz-5.8.3/src/liblzma/common/alone_decoder.c
../../../xz-5.8.4/src/liblzma/common/alone_decoder.c
```

In all 18 relocation-only input comparisons, substituting the version in
the recorded debug-input text removed every difference. This substitution
was analysis only; it never changed a compiler invocation, source, cache
input or qualification criterion. The real upgrade retained other differences,
including changed declarations in `common.h` and error cleanup code in
`alone_decoder.c`. Those misses cannot all be attributed to paths, and
path normalization alone would not guarantee reuse of these real upgrades.

The base-directory profiles also changed compiler-visible
paths before the existing absolute Clang prefix maps could match them.
`llvm-dwarfdump` showed the uncached new object's `DW_AT_name` as
`/openmktr-src/src/liblzma/common/alone_decoder.c`, versus
`../../../xz-5.8.4/src/liblzma/common/alone_decoder.c` with `CCACHE_BASEDIR`.
All 12 mismatching objects in each workload became byte-identical to their references after
`llvm-objcopy --strip-debug` in separate diagnostic copies. That explains
the differences; stripping debug data is not an accepted repair or a weaker
qualification criterion. These were cache misses followed by changed
compiler path behavior, not evidence of stale cache hits or changed machine
code.

Diagnostics matched in all 36 comparisons. All 18 emitted new-state
depfiles resolved to exactly the same existing input paths as the uncached
reference; textual absolute/relative spelling differed. No cross-version
restoration succeeded, so this establishes no post-restoration incremental,
file-macro, link, runtime or end-to-end performance qualification for the
new base-directory profiles. Further safety fixtures and the C++ benchmark
were deliberately not run after this failed feasibility gate.

**Recommendation: stop further cross-version cache work for now.** Retain
the optional harness and evidence. Reopen only for a concrete costly
workload that justifies another separately scoped experiment. Neither these
misses nor the original slowdown justify a custom cache implementation.

The [follow-up results](cross-version-path-results.json) retain every
comparison, per-call counters, input identities and the explicit stop
decision. Full evidence, including original objects, depfiles, DWARF dumps,
ccache debug inputs and old/new input diffs, remains under
`validation/logs/cross-version-followup-20261024/xz-probe-final/` and
`validation/logs/cross-version-followup-20261024/xz-relocation-control/`.
The immutable real-upgrade run used the first probe snapshot; the subsequent
snapshot added the relocation-only control and computed the common parent
from each recorded path. Both snapshots and their hashes are retained.
Eight portable accounting/failure tests and eight Portage evidence tests
passed in Chimera with the current checkout mounted read-only. No C++ runtime
code changed; the full runtime gate was not rerun for this follow-up.

To reproduce, first retain the original campaign container and its matching
evidence. The probe requires both versioned configured trees at the paths in
the compiler journals. If those are unavailable, recreate a passed xz campaign
using the preceding reproduction procedure with `--pair xz-utils --mode ninja
--profile normalized --repeats 1`, retaining its container and evidence.
Then run the probe in a disposable filesystem copy:

```sh
docker commit omktr-cross-version-qualified openmktr-reuse-path-inputs
image_id=$(docker image inspect --format '{{.Id}}' openmktr-reuse-path-inputs)
mkdir -p followup-evidence
docker run --rm -e OMKTR_PORTAGE_VALIDATION=1 \
  -e OMKTR_VALIDATION_IMAGE_ID="$image_id" \
  -v "$PWD:/candidate:ro" \
  -v "$PWD/validation/logs/cross-version-20261024/qualified:/prior:ro" \
  -v "$PWD/followup-evidence:/followup" \
  --entrypoint python3 "$image_id" \
  /candidate/validation/compatibility/cache_path_probe.py \
  --prior /prior --output /followup/xz-probe
```

Use the corresponding retained container name and evidence path if
recreating the input campaign. Each invocation requires a fresh output and
workspace. Repeat in another disposable copy with `--relocation-only` and
`--output /followup/xz-relocation-control` for the identical-content control.
The recorded derived image was
`sha256:e7b211046f7fecc9c0d32c270179ea167f119bfdb927d33279ef9d5e3efc3ba3`.
This probe executes compiler calls only; it does not run Portage phases or
need the namespace permissions used by the full package campaign.

### Validation and evidence correction

The local Chimera gate passed C++ units, version tests, correctness and
compatibility suites, adapter/load tests, evidence tests, all 858 snapshot
scenarios with zero quarantined crashes, and the include-dump smoke check.
The cache campaign remains outside that GNU-free gate. Remote CI was not run.

After measurement, the concurrent fixture gained explicit object/artifact/
depfile comparisons for both simultaneous builds, and relocation cases gained
positive-hit assertions. Pre-test compile inventories were also explicitly
rechecked against their uncached references for every measured build.
The 16 affected controlled mode/profile combinations were rerun
successfully. The shared evidence helper also gained a parameter so this
campaign does not claim an Openmktr ebuild `USE=test` installation: it uses a
selected binary validated separately. Six accounting tests cover that marker,
failure detection and seed isolation; the existing eight Portage evidence
tests passed too.

The immutable original raw run contains the shared helper's inherited
`openmktr_use_test=true` marker. It is not evidence of an installation test
and is corrected to false in the checked-in results, with an explicit
erratum. Original timing values, comparison outcomes and archived source
hashes are retained. Current source hashes and the supplemental validation
are recorded separately. The compiler wrapper, runtime binary, cache policy
and timing procedure were unchanged by these post-measurement checks.

## Qualification contract

The initial experiment pins ccache **4.14**, Clang, a Gentoo repository
snapshot, a container image and an Openmktr binary. It builds gzip
`1.14-r1 -> 1.15` and xz-utils `5.8.3 -> 5.8.4` through real Portage phases in
both direct and Ninja modes. Builds end at staged installation; they do not
replace host or container system packages with the benchmark outputs.
Dependencies and downloads are prepared separately. The actual package
ebuilds, patches, and release archives remain unchanged.

The qualified configuration uses preprocessor mode, compiler-content checking,
directory hashing, no sloppiness, no hard links, no remote cache, and an empty
`CCACHE_BASEDIR`. Only ordinary C/C++ compilation is considered. Assembly, PCH,
modules, plugins, and profile-guided compilation are excluded; recognized
unsupported invocations bypass ccache. This is a qualification of the listed
inputs, not an exhaustive detector for arbitrary compiler side inputs. A
fixed toolchain and stable build inputs during each compiler invocation are
required. An executable hash alone cannot identify all shared compiler
libraries, resource files, or sysroot contents; the pinned environment is part
of the contract.

Ccache's direct mode has a documented newly appearing header limitation. It
is deliberately excluded from these results. Never enable ignored headers,
ignored options or timestamp sloppiness to improve this campaign's hit rate.
See [ccache 4.14](https://ccache.dev/manual/4.14.html#_the_direct_mode).

Each experiment has a private writable cache configured for 2 GiB automatic
cleanup, outside `WORKDIR`. An immutable copy of the old-version seed is kept
while its comparisons run, so disk use includes both copies plus evidence and
build trees. Completed seed copies are removed. This is a ccache size policy,
not a filesystem quota or a promise about instantaneous disk consumption.
Only trusted build users may write the cache.

## Workloads and controls

The campaign defaults to four jobs, one package build at a time and five
repetitions. Every new-version build starts from Portage `clean`, with fresh
objects, generated headers, Ninja graphs and completion stamps. Only compiler
cache contents cross the version boundary. `--recursive_cache` is disabled.

Each repetition compares:

| State | Initial compiler-cache contents |
| --- | --- |
| `disabled` | Compiler runs without ccache. |
| `cold` | Empty cache; same-build duplicate compilations may subsequently hit. |
| `upgrade` | A fresh copy of results from the old version only. |

The order reverses on alternate repetitions. No new-version results carry
into the next upgrade sample. Cold and upgrade samples must match the same
repetition's uncached object hashes, executable/library hashes, dependency
files, and compilation diagnostics. Upstream test reports must contain tests;
compression round trips and a client linked to staged liblzma must pass.
Any unexplained mismatch or missing evidence fails the campaign.

Dependency files are compared where emitted and their counts are recorded.
Gentoo's unmodified package configuration can disable dependency tracking;
an empty dependency inventory is not evidence of incremental dependency
coverage. The controlled fixtures explicitly emit depfiles and test header
changes after restoring objects in both execution modes.

Two compiler path profiles are independent experiments:

- `original`: original compiler-visible paths and `-O2 -g`.
- `normalized`: explicit Clang source/build prefix mappings plus
  `-fdebug-compilation-dir=.`. The uncached control receives exactly the same
  mappings. `__FILE__` and debug path behavior can change, so this profile
  requires explicit selection outside the investigation.

The mappings do not rewrite ccache's dependency files or preprocessor line
markers. In particular, a compiler prefix map is not proof that all paths in
a cache key have been normalized. Absolute source/include paths may still
prevent reuse. `CCACHE_BASEDIR` remains unset rather than introducing its
documented dependency-path rewriting concerns without qualification.
See [Clang's flag reference](https://clang.llvm.org/docs/ClangCommandLineReference.html)
and [ccache's directory discussion](https://ccache.dev/manual/4.14.html#_compiling_in_different_directories).

The controlled C/C++ suite covers relocation and revisions, source changes,
patch results, USE-style definitions, options, compiler identity, generated,
transitive and system headers, preserved mtimes, newly appearing headers,
include precedence, dependency version/ABI changes, static/dynamic libraries,
debug paths, file/time macros, recursion, parallelism, compiler failures,
missing/corrupt entries, eviction, concurrent writers and rebuilding from
restored depfiles. A separate EAPI 8 fixture applies an actual `src_prepare`
patch in revision `1-r1`, then compares cached and uncached results.

Changing a dependency version does not by itself prove every object invalid.
A changed header/ABI must affect the corresponding compilation; a library-only
change may permit object reuse. Configuration, generators, linking, upstream
tests and installation always execute normally.

## Reproduce

Run only inside a disposable Gentoo container. This external environment may
use Gentoo's Bash, glibc and package tools; it is separate from the portable
Chimera gate. Host configuration is never a destination for this campaign.

First build the existing Gentoo compatibility image from the checkout:

```sh
docker build -f validation/compatibility/Dockerfile.gentoo \
  --build-arg OMKTR_SOURCE_REVISION="$(git rev-parse HEAD)" \
  -t openmktr-gentoo .
```

Prepare tools and benchmark prerequisites in a new container without syncing
its Gentoo snapshot. Build the candidate with the reference Ninja path and
copy it to the campaign's selected runtime location:

```sh
docker run --name openmktr-reuse-prepare --entrypoint sh openmktr-gentoo -c '
  set -e
  . /etc/profile
  mkdir -p /etc/portage/package.accept_keywords /etc/portage/package.use
  printf "%s\n" "=dev-util/ccache-4.14 ~amd64" \
    "=app-arch/gzip-1.14-r1 ~amd64" "=app-arch/gzip-1.15 ~amd64" \
    "=app-arch/xz-utils-5.8.3 ~amd64" "=app-arch/xz-utils-5.8.4 ~amd64" \
    > /etc/portage/package.accept_keywords/openmktr-cache-preparation
  printf "%s\n" "app-arch/gzip -static pic" \
    "app-arch/xz-utils -pgo -doc -nls -static-libs abi_x86_64 -abi_x86_32" \
    > /etc/portage/package.use/openmktr-cache-preparation
  emerge --oneshot --getbinpkg --jobs=4 --autounmask=n =dev-util/ccache-4.14
  emerge --oneshot --onlydeps --getbinpkg --autounmask=n \
    =app-arch/gzip-1.14-r1 =app-arch/gzip-1.15 \
    =app-arch/xz-utils-5.8.3 =app-arch/xz-utils-5.8.4
  python3 packaging/gentoo/configure_build.py
  ninja-reference -f build.portage.ninja -j4 omktr
  mkdir -p /runtime
  cp omktr /runtime/omktr
'
docker commit openmktr-reuse-prepare openmktr-reuse-prepared
```

Commit that prepared container to a local image and retain its full immutable
image ID. The [recorded result](cross-version-reuse-results.json) identifies
the prepared image used for this investigation. A moving image tag alone is
not sufficient provenance. If a pinned ebuild or ccache version is absent,
stop rather than substituting another release or editing the release sources.

From the host checkout, mount the checkout read-only at `/candidate` and an
empty evidence directory at `/evidence`. The checkout mount supplies Git
revision information and the investigation sources; the selected binary must
have been built from the same runtime sources.

```sh
image_id=$(docker image inspect --format '{{.Id}}' openmktr-reuse-prepared)
mkdir -p reuse-evidence
docker run --rm --cap-add SYS_ADMIN --cap-add NET_ADMIN \
  --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
  -e OMKTR_PORTAGE_VALIDATION=1 -e OMKTR_VALIDATION_IMAGE_ID="$image_id" \
  -v "$PWD:/candidate:ro" -v "$PWD/reuse-evidence:/evidence" \
  --entrypoint sh "$image_id" -c '
    . /etc/profile
    exec python3 /candidate/validation/compatibility/cross_version_cache.py \
      --omktr /runtime/omktr --output /evidence/run
  '
```

Use `--pair gzip` or `--pair xz-utils`, `--mode direct|ninja`,
`--profile original|normalized`, `--jobs N` and `--repeats N` for focused
runs. Repeat pair/mode/profile options to select multiple values.
`--fixtures-only`, `--packages-only`, and repeatable `--fixture NAME` support
investigation without claiming omitted coverage. A fresh container and output
directory are required for each invocation. Namespace-sandbox failures,
timeouts, compilation failures and comparison failures are retained and
reported as failures.

The privileges permit Portage's namespace sandboxes to run in the disposable
container; namespace failures are not silently downgraded. See the existing
[Portage lane](../validation/compatibility/README.md#opt-in-portage-integration)
for the reason for each container permission.

## Evidence and interpretation

`result.json` records running/passed/failed state, configuration, image and
binary identities, source hashes, host limits, all samples and summaries.
`commands.jsonl` records commands, exit statuses, timeouts and durations.
Per-build compiler journals retain arguments, paths, diagnostics, output
hashes, unsupported invocations and CPU/wall measurements. Package manifests,
archive digests, upstream reports, configuration logs and Ninja graphs remain
beside them. The tooling sources are copied into each evidence directory.

`build_seconds` spans unpack through staged installation, including the
intervening accounting work. `phase_total_seconds` sums the phase-command
durations. Compilation is also reported separately. Downloads, prerequisite
installation, cache-seed copying and final smoke/artifact verification are
outside those timers. Compiler instrumentation is enabled for all controls;
these timings are for instrumented builds on the recorded machine. Filesystem
caches are not flushed. Median, minimum, maximum and every raw sample are
retained; small differences have no confidence-interval claim.

`compile_seconds` covers the Portage compile-phase command, including Ninja
generation when selected, scheduling and compiler instrumentation. Individual
compiler-call CPU and wall measurements are retained in the journals; the
compile-phase duration is not a sum of compiler process times.

Cache counters are separated by phase. Configure-probe hits and duplicate
compilations within one build must not be counted as useful cross-version
package-object reuse. After measurement, one corresponding old/new compilation
is replayed with ccache debug logging and forced recomputation. Its hashed
input diff helps distinguish path misses from actual code/configuration
changes; those diagnostic runs are outside the measurements.

The decision criterion is measured benefit with correct outputs, without a
fixed speedup threshold. Low benefit calls for explaining misses and time
spent outside compilation. A correctness failure disqualifies that
configuration. A custom artifact layer requires a separate proposal showing
valuable work that existing tools cannot safely reuse, a narrow action set,
complete input/output tracking, bounds, atomic publication and conservative
fallback. A low aggregate hit rate alone does not justify it.
