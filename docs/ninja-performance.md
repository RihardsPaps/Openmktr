# Ninja performance and retained workspace evidence

The implementation improves measured incremental workloads and adds optional
version-independent workspaces with idle compression. The full FFmpeg/Linux
performance objective remains **unverified**: FFmpeg clean-build changes are
small, and compiler crashes prevented a complete Linux comparison. Graph size
alone is not an acceptance result.

The [machine-readable report](../validation/ninja-performance-results.json)
retains individual samples, warmups, command templates, configuration and artifact
hashes, distributions, paired changes, failures and exclusions. Raw command
journals, build logs, compiler crash reproductions and validation logs are retained
locally in `validation/logs/ninja-performance/`; they are ignored by Git. The report
identifies their hashes. Original measurement paths are recorded; disposable
build/source copies are removed after saving and verifying the raw evidence.
These results describe this builder and configuration.

## PR candidate recheck, 26 October 2026

The submitted native candidate is identified by SHA-256
`272872a9169d6791c315ba23ded1f9a64612133b8cca439ff29d5a6618c9ad0a`.
It includes the later assignment-operator, background-recipe, workspace-export
and dummy-recipe identity fixes. The helper also includes subsequent source-sync
fixes. The [current candidate report](../validation/pr-performance-results.json)
records raw and LF-normalized source hashes, binary hashes, all warmup/measured
samples, commands, output checks, summaries and a retained raw-log archive hash.
The historical sections below use “final” for the earlier `a7cea354` binary;
their results remain separate from this recheck.

The baseline remains `bf92f53960d0b099f64dd4db8690cb7e823ac342`, binary
`43a4af2132f9b1cb3e6d64ab7f2a20d8165c34c0b81e7e93a1f99e0fb4ec2356`.
Both tools use the same Clang/libc++ 22.1.8, musl, O2/ThinLTO/static toolchain
and link flags described below. The current candidate was rebuilt from all native
objects in the retained toolchain image. Version banners carry explicit baseline
revision overrides; binary and source hashes identify the actual candidate.
No local tests, builds or profiles ran concurrently with the timed workloads.

The complete 512-output synthetic matrix covers both entrypoints at j8 and j16,
with two discarded warmup pairs and seven alternating measured pairs each.
Clean paired median improvements were **20.41–25.34%**, and incremental scenario
improvements were **7.86–23.39%**. Every interval was positive. These short
copy/printf recipes measure invocation and recipe overhead.

The current FFmpeg 7.1.1 recheck covers `omktr build -j16`, with the same two
warmup pairs and seven measured pairs. The full current j8/legacy matrix was
not rerun. Configuration and verification are outside invocation timers.

| FFmpeg scenario | Baseline median | Candidate median | Paired median improvement | 95% interval |
| --- | ---: | ---: | ---: | ---: |
| Clean | 32.2514 s | 32.1644 s | 0.48% | 0.27% to 0.72% |
| No-op | 4.0979 s | 3.9246 s | 4.65% | 3.99% to 6.04% |
| Touched compiled source | 4.0975 s | 3.9369 s | 3.55% | 2.48% to 5.87% |
| Missing object | 4.1298 s | 3.9089 s | 4.91% | 3.80% to 5.71% |
| Makefile invalidation | 3.7855 s | 3.6026 s | 4.98% | 3.55% to 5.11% |

Percentages are medians of paired ratios, rather than ratios of the two time
medians. Clean improvement is small; these results do not establish a universal
compiler-throughput gain. No supported regression appeared in this recheck.
All FFmpeg/ffprobe hashes matched across every sample and scenario, encode smoke
tests passed, candidate no-op object recompilations were zero, required changed
or missing objects rebuilt, unrelated object timestamps stayed intact, and release
source contents remained unchanged.

The seven-repeat `tests/bench.py` comparison also completed in Chimera. Its fixed
candidate-then-baseline ordering is supplemental evidence. Conversion medians
were 0.064975 s baseline and 0.053812 s candidate; sleep-based parallel workloads
were effectively unchanged. Binary size rose from 1,853,088 to 1,868,280 bytes
(0.82%). Full overhead, memory, phase and size results are in the current report.

Fresh validation of this source passed the supported Chimera gate: four C++
units, version tests, **173 correctness cases**, six Make compatibility cases,
Portage/load/evidence/cache suites, three benchmark-accounting cases,
**22 workspace cases**, six packaging cases, **858/858 snapshots** with zero
quarantined crashes, and the dump smoke test. One existing Portage case was
skipped. Fresh required ASan/UBSan units and Portage/load/correctness/workspace/
compatibility suites passed with CI's leak-detection setting. The five portable
incremental-harness cases, changed C++ formatting, actionlint checks of all three
changed workflows, and diff checks passed. This records local validation;
remote checks are reported on the PR.

Gentoo's first PR run exposed an environment assumption in the new deferred
assignment test: Portage exports `CFLAGS=-O2 -pipe`, while the test requires an
initially undefined child variable. The fixture now clears and restores that
test-process variable. All 173 correctness cases passed with Portage's CFLAGS
after the correction. This follow-up changes tests only; native and helper code
are unchanged, so the measured binary and results still apply. The current
report preserves the measurement snapshot and identifies the later test hash.

The complete Linux matrix remains unverified. The NAS comparison was still
running with the earlier `a7cea354` candidate when this report was prepared and
does not establish acceptance for the submitted candidate. Kernel boot, the full
current optional Gentoo/Alpine workflow and a full real-package workspace campaign
were not run locally. Earlier Portage and compression observations below retain
their original binary/helper identities.

## Changes retained

* Skip redundant VPATH filesystem probes when a prerequisite's existing rule or
  resolved graph node already determines its spelling. Dependencies and ordering
  remain present.
* Capture interruption-cleanup timestamps in the existing recipe supervisor,
  after job admission. This removes a separate Openmktr process and generated
  shell cleanup wrapper per file recipe. Changed non-precious outputs are still
  removed after interruption, including recipes with their own signal traps.
* Preserve literal saved compiler commands such as Kbuild's `$(pound)` data.
  Deferred quoted assignment expansion is limited to shell assignment prefixes;
  Ninja serialization does not expand the command a second time. The baseline
  incorrectly shortened saved objtool commands and rebuilt during no-op runs.
* Add [managed workspaces](workspaces.md) that retain source and build state
  across version-directory changes. Configuration/compiler profiles isolate
  incompatible state. Unchanged inputs keep their timestamps; changed inputs,
  depfile inputs and recipes rebuild. Failed or interrupted synchronization
  requires explicit recovery before building.
* Compress inactive workspace contents with verified xz archives, retaining exact
  timestamps, modes, symlinks, objects and Ninja state. Hot builds stay expanded.
  Portage integration is opt-in and has phase ownership checks.

Opaque recursive shell recipes remain opaque. No dependencies were removed on
the basis of transitive reachability, and the experimental recursive cache is
not enabled by these changes. Existing ordinary build commands and defaults stay
available; `omktr workspace ACTION` selects the optional workspace frontend.

## Earlier binaries and method

Baseline revision: `bf92f53960d0b099f64dd4db8690cb7e823ac342`.
Working branch: `codex/ninja-build-performance`.

| Binary | SHA-256 |
| --- | --- |
| Baseline | `43a4af2132f9b1cb3e6d64ab7f2a20d8165c34c0b81e7e93a1f99e0fb4ec2356` |
| Core performance candidate | `c040bb65f2591621d94a73f80ea4c365bcb0795cd55eb153fdc920077bf54141` |
| Earlier native candidate | `a7cea354bf648cb889a1f74f753d69d66baf37bdfeffe2642d77789e29cc3212` |
| Earlier workspace helper | `2b397d974a7a2d1af76bac6354c5ed502f96657d26c712be6b42e978c584420e` |

Native baseline and candidates were rebuilt with identical Clang/libc++ 22.1.8,
C++17, `-O2`, ThinLTO, function/data sections, lld garbage collection and identical
code folding, and static musl linkage. The static musl development/header packages
were `1.2.6-r3`; the Chimera image's runtime musl package was
`1.2.5_git20240705-r12`. Full flags and the final link command are in the JSON.
Version banners share the explicit baseline revision override; binary hashes
identify candidates. An earlier diagnostic binary linked without the matched
garbage-collection/folding options is excluded from performance evidence.

External FFmpeg and initial kernel recipes used Ubuntu 24.04 under WSL,
Clang 18.1.3, Ninja 1.11.1, and glibc. These release prerequisites are isolated
from the supported GNU-free Chimera build/test gate. The LLVM 19 kernel retry
used its separate campaign container and is not pooled with LLVM 18 timings.

Each complete matrix uses `-j8` and `-j16`, `omktr build` and legacy
`omktr --ninja --regen` followed by `sh ninja.sh`, two warmup pairs and seven
measured pairs. Baseline/candidate order alternates. Every pair starts in fresh
build directories and then performs clean, no-op, touched compiled source,
missing object and Makefile/include invalidation scenarios. Release source
contents remain unmodified; the touched source timestamp is restored. Synthetic
fixtures additionally check actual source/include content changes.

Compiler caches are disabled. Filesystem caches stay warm; the process affinity
allows 32 logical CPUs, without host frequency isolation. Configuration, source
hashing, object scans, artifact checks and smoke tests are outside invocation
timers. Configuration is recorded separately. Legacy generation/regeneration and
recipe execution have separate timers; `omktr build` is timed as a complete
invocation. Instrumented graph/process/recursive diagnostics are separate runs.

Paired improvement is `100 * (1 - candidate / baseline)`. The summary uses the
median of paired percentages and a seeded 10,000-resample bootstrap interval.
Separate medians of elapsed time need not give the same ratio. Seven completed
pairs and an interval wholly above zero are required for a supported improvement.
This small-sample interval describes observed variation, not a universal guarantee.

FFmpeg used unmodified 7.1.1 with Clang and
`--disable-autodetect --disable-doc --disable-debug`. Linux used unmodified 6.12
with `ARCH=x86`, `defconfig`, version-suffixed LLVM tools and `LLVM_IAS=1`.
Archives were:

| Source | SHA-256 |
| --- | --- |
| FFmpeg 7.1.1 | `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1` |
| Linux 6.12 | `b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb` |

## Completed measurements

The core candidate completed the full FFmpeg matrix. Incremental paired median
improvements range from 3.31% to 5.76% across both entrypoints and job counts,
with positive intervals. Clean build/j8 improved 0.81% [0.58%, 1.15%]; legacy
clean changes were within variation. Clean build/j16 initially regressed
0.45% [-2.23%, -0.23%], so an independent final-binary recheck was required.

That isolated final recheck completed seven measured pairs after two warmups:

| FFmpeg, final candidate, `omktr build -j16` | Paired median improvement | 95% interval |
| --- | ---: | ---: |
| Clean | 0.19% | -0.65% to 0.33% |
| No-op | 3.62% | 2.46% to 4.83% |
| Touched compiled source | 3.67% | 3.37% to 4.79% |
| Missing object | 5.05% | 4.86% to 5.45% |
| Makefile invalidation | 5.21% | 4.38% to 6.89% |

The earlier clean regression did not repeat, but the final clean result also
does not establish a gain. A prior final FFmpeg recheck overlapped a broad
filesystem lookup during one sample; all its rows are retained but excluded from
acceptance. The isolated repeat above is the reported recheck. The full final
FFmpeg j8/legacy matrix was not rerun after workspace support was added.

The final binary completed the full controlled 512-file fixture matrix: clean
paired median gains were 19.12% to 25.69%; incremental gains were 6.64% to 25.85%,
all with positive intervals. This demonstrates reduced invocation/recipe overhead
for short recipes, rather than the same percentage benefit for long compilations.

Successful release samples verify equal baseline/candidate FFmpeg executable
hashes, unchanged deterministic artifacts after incremental scenarios, zero
candidate no-op object recompilations, required source/missing-output rebuilds,
and preserved unrelated object timestamps. Every completed FFmpeg build also
runs the existing synthetic-video smoke command. Source content checks pass.

## Incomplete kernel comparison

Four WSL full-matrix attempts stopped on Clang frontend exit 139 in the baseline:

| Attempt | Environment | Failure point |
| --- | --- | --- |
| `linux-final` | Clang 18.1.3 / WSL | j8 build, first warmup; `cpuhotplug.c` |
| `linux-final-r2` | Clang 18.1.3 / WSL | j8 build, fourth measured pair; `fs/kernfs/file.c` |
| `linux-llvm19` | Clang 19.1.1 / Docker, 64 MB stack | j8 build, first warmup; `shmem.c` |
| `linux-llvm19-investigated` | Same image and stack, fresh WSL campaign | j8 build, second warmup; `addrconf.c` and `hid-core.c` |

The exact first Clang command replayed successfully outside Openmktr at both
8 MB and 64 MB stack limits. The cause remains unresolved; changing the compiler
and stack did not establish a cure. Compiler traces, a preprocessed LLVM 19
reproduction and the failed command journals are retained. No third-party source
or kernel optimization flags were patched to obtain a pass.

Only three measured pairs completed in the second attempt. They suggest a large
no-op reduction, consistent with avoiding objtool recompilation, but they fail
the seven-pair requirement and are **exploratory only**. Successful kernel pairs
matched `vmlinux`/`bzImage` hashes and required incremental output checks; no
kernel was booted. The missing completed j8/j16 and legacy matrix prevents
claiming the requested full real-project clean/incremental acceptance.

The separate LLVM 19 image ID was
`sha256:42bae58178c98432973444c1b5b7de9b5e847f1fce7372c73a1034749902c050`.
Its [Dockerfile](../validation/ninja/Dockerfile.llvm19) isolates external GNU
prerequisites; image metadata and explicit stack limits are in the report.

The [follow-up crash investigation](../validation/ninja-clang-investigation-results.json)
reproduced an independent Clang failure through GNU Make, with Openmktr absent
from the build. Five clean kernel reference builds passed with identical artifact
hashes; the sixth aborted in Clang while parsing `net/ethtool/linkinfo.c`, reporting
"stack smashing detected" and frontend exit 134. All 672 standalone compiler
replays passed, including the original commands against the failed builds'
generated headers. Source contents remained unchanged. The evidence establishes
that these compiler failures are not specific to Openmktr; it does not identify
whether the underlying mechanism is in the compiler, WSL or hardware.

An independent full comparison is running on the configured Ryzen 7 5800X NAS
using the exact same pinned image, baseline/candidate binary hashes and stack
limits. Its first baseline kernel artifacts match the independent GNU reference
hashes. NAS timings are separate from WSL timings. Existing NAS services remain
running; shared host load and lower benchmark CPU scheduling weight are disclosed
in the investigation report. Completed timing acceptance will be recorded after
all seven measured pairs in each configuration finish.

## Remaining overhead and diagnostics

In a separate 200-file diagnostic, baseline/final process launches fell from
1,009 to 809, and output-stamp helper launches from 200 to zero. Both retained
200 recipe supervisors, 201 graph nodes and 400 dependency edges. Generated
graph bytes fell from 196,571 to 95,481; absolute executable-path spelling also
contributes to those byte counts. Output checks passed.

For a recorded full-matrix FFmpeg legacy no-op pair, regeneration/check time was
3.799 s baseline versus 3.609 s candidate; recipe execution was 0.396 s versus
0.390 s. Regeneration remains the largest measured no-op cost, while clean
release builds predominantly execute compilation recipes. Further improvements
need profiling of regeneration rather than graph-size targets.

Separate nested-recursion diagnostics retained two opaque shell loops, all 16
workers and exports. Observed worker peaks were one at `-j1` and four at `-j4`.
The existing graph recursive counter recognizes legacy `KATI_DEPTH` markers
and reported zero here; the retained worker events establish actual recursive
scheduling. Normal and sanitizer FIFO tests confirmed token return after both
successful and failed recipes.

## Workspace storage and correctness

The final helper compressed the Linux 6.12 source-only regular-file payload from
1,476,498,481 bytes to 158,929,912 bytes, saving 89.24%. This ratio does not include
compiled objects; packages and configurations have different compression ratios.
Packing took 301.26 s and restoration 25.52 s on this builder. Every archived file
was verified before deletion and after restore, and a checked source timestamp
was restored exactly. These cold-cost observations may overlap validation work
and are not timing acceptance samples. A 0.036 s hot frontend `/bin/true` probe
measures frontend overhead only, not a kernel build.

The 18-test workspace suite covers real Clang compilation, depfile-only headers,
changed recipes, byte-equal clean controls, no-op reuse after restore, version
changes, removed inputs, compiler/configuration isolation, failures, cancellation,
interrupted synchronization/recovery, generated sources, directory modes,
corruption and unsafe archives, hardlinks, private metadata and active-owner GC.
Shared behavior is tested in direct and Ninja modes where applicable.

An actual native EAPI 8 Portage fixture installed three prepared releases using
the final native binary and helper. Release two retained object hashes and exact
timestamps; after compression, release three changed `a.o` and the installed
result while leaving `b.o` unchanged. Normal Portage cleanup removed original
version directories. Active ownership, root maintenance privilege dropping and
idle ages were checked in disposable containers. A complete earlier overlay
ebuild install with `USE=test` also passed; its separate helper hash is retained.
The optional full real-package workspace campaign was not run. Packages with
absolute original source paths, custom cleanup or incomplete dependency tracking
need qualification as described in the workspace guide.

## Validation and reproduction

The earlier supported Chimera image gate passed: four C++ units, version tests,
170 correctness cases, Make compatibility, Portage adapter/load/evidence/cache
suites, benchmark accounting, 18 workspace tests, six release packaging tests,
858 snapshots and the dump smoke test. One existing Portage case is an expected
skip. Required ASan/UBSan units and correctness/compatibility/Portage/load suites
passed, with the final 18 workspace tests additionally run against the sanitizer
binary. Changed C++ formatting and `git diff --check` pass. Incremental campaign
harness tests and normal/sanitized Ninja FIFO tests pass. Remote CI was not run.
The optional workflow jobs were not dispatched. The follow-up checked all three
changed workflows with the available actionlint container; those checks pass.

Run the portable checks from the contributor guide. With matched baseline and
candidate binaries plus existing unmodified archives on a Linux campaign host:

```sh
python3 validation/benchmark_ninja.py \
  --root /tmp/omktr-paired-ffmpeg \
  --archives /path/to/release-archives \
  --baseline /path/to/baseline --candidate /path/to/candidate \
  --baseline-revision BASELINE --candidate-revision CANDIDATE \
  --project ffmpeg --jobs 8 16 --warmups 2 --samples 7
```

Use a fresh root for each run, repeat with `--project linux`, and supply
`--llvm-version 19` for version-suffixed LLVM 19 tools. The harness defaults to
both entrypoints. Preserve failed runs; do not reduce sample counts or count
partial pairs as accepted comparisons. `python tests/bench.py CANDIDATE
--baseline BASELINE --repeats 7` was also run as supplemental microbenchmark
evidence; its fixed candidate/baseline ordering is not the primary paired method.
