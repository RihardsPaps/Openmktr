# Portage integration qualification — 2026-10-24

The four review findings and pre-release improvements are implemented. These
are local Docker results for the uncommitted candidate on baseline
`ba63336927c757ade31a929c1a140d8916fd6ea8`, not a GitHub CI result or a claim
of compatibility with arbitrary Gentoo packages.

## Fixes covered

- Gentoo binaries invoke `/usr/bin/ninja-reference` explicitly, with Ninja
  1.13 or newer declared as a runtime dependency. Tests poison the ordinary
  `ninja` path, and the real lifecycle runs with that alternative set to Samurai.
- Native per-launcher load admission allows progress with one recipe, polls
  changing samples, and warns once and serializes after sampling fails.
  Process-owned slots release on exit; recursive executors retain their own
  progress exception and shared FIFO budget.
- Launcher argument handling preserves `--`, option operands, Ninja tools,
  explicit resets, and inherited reservations. Finite representable subnormal
  load limits are accepted.
- Evidence directories cannot be reused. Atomic running/passed/failed results,
  timeout records, process-group cleanup, source identity, and image identity
  make unsuccessful runs distinguishable from successful ones.
- Release sanitizer coverage matches the adapter/load checks in PR CI.
  Documentation covers Portage configuration layouts, installed examples,
  optional tests, and wrapper symlink behavior; environment examples retain LF.

## Chimera checks

The reference image used Clang/LLVM 22.1.8, musl/libc++, Ninja 1.13.2, and
Python 3.14.6. Image identity:
`sha256:7d7799a5528d793f267de60d36e52e485a45925fe448856536a098786e098fd6`.

Passed:

- Native build and all four C++ unit binaries; changed-C++ formatting.
- Version generator: 2 tests; correctness: 164; Make compatibility: 6.
- Adapter: 9 passed and 1 expected Gentoo-configuration-only skip.
- Load limits: 13 tests, including controlled positive-load transitions,
  failures after successful sampling, recursive/nested Ninja progress,
  cancellation, FIFO return, launcher arguments, and temporary-state cleanup.
- Snapshot regression: **858/858**, no quarantined crashes, no snapshot changes.
- Include-dump smoke and Ninja FIFO integration: passed.
- ASan/UBSan build, four unit binaries, adapter/load/correctness/compatibility
  suites, and Ninja FIFO test: passed with `ASAN_OPTIONS=detect_leaks=0`.
- Final evidence-harness tests: 7 passed. These were rerun separately after
  the last harness-only fixes; the full image run had the earlier 5 tests.
- Incremental campaign harness: 5 passed; release packaging: 6 passed.
- External GNU Make differential campaigns at `-j1` and `-j4`: no correctness
  failures, unnecessary rebuilds, or incomplete comparisons.
- Workflow actionlint and `git diff --check`: passed.

The synthetic benchmark compared the candidate with baseline `ba633369` built
using the same Chimera compiler and flags. Three-sample and five-sample reports
are retained as `benchmark.json` and `benchmark-repeat.json`. An initial nested
parallel timing increase did not repeat; the five-sample medians for flat
conversion, execution, no-op, and recipe-launch cases were within approximately
5% of baseline. These small, host-sensitive samples do not establish a general
performance guarantee; load limiting intentionally reduces concurrency.

The first local sanitizer invocation appended a carriage return to its final
command through PowerShell stdin. A complete rerun using an LF script passed;
`chimera-sanitizer-2.log` is the qualifying sanitizer record.

## Real Gentoo/Portage lifecycle

The final `--real-packages` run passed with Portage 3.0.82.2, Gentoo amd64/23.0,
glibc 2.43-r4, Clang/LLVM 22.1.8, libc++, Ninja 1.13.2, and Python 3.14.7.
Portage's IPC, network, PID, and filesystem sandboxes remained enabled.
Docker used `SYS_ADMIN`, `NET_ADMIN`, and `seccomp=unconfined` as documented in
the [lane guide](README.md#opt-in-portage-integration).

The live Openmktr ebuild built from candidate source, ran `USE=test` (including
all 10 adapter tests and 7 evidence-harness tests), installed, and unmerged.
An unselected control package and direct/Ninja selections passed configure,
compile, tests, staged install, merge, and unmerge. Both default `check` and
fallback `test` targets were exercised. Parallel limits, regeneration, graph
placement, and the absence of automatic activation files were checked. Both
intentional compile failures failed before merge. The final ledger contains
39 commands with the expected outcomes, including exactly two expected failures.

The following unmodified Gentoo ebuilds were additionally built from source,
tested, and merged through `emerge` in **both direct and Ninja modes**:

| Package | Upstream result in each mode | Installed-artifact check | Recursive Automake |
| --- | --- | --- | --- |
| `app-misc/hello-2.12.2` | 6 passed, 1 skipped, no failures | Installed `hello` output | No |
| `dev-libs/libpipeline-1.5.8` | 7 passed, no skips/failures | Compiled and ran a client linked to the installed library | Yes |
| `app-arch/gzip-1.14-r1` | 30 passed, no skips/failures | Installed compression/decompression round trip | Yes |

Hello's upstream `greeting-2` test skipped because it was not a full moon night.
No release sources or repository ebuilds were patched to make the checks pass;
normal preparation performed by Gentoo's ebuilds remained intact.

Portage clean runs before each package/mode with `keepwork` and `keeptemp`
temporarily disabled. This is necessary because retained completion stamps
otherwise let a second mode skip compilation and tests. The harness verifies
actual configure, compile, test, and install phase markers and requires the
adapter within compilation. It retains configured Makefiles, Ninja graphs,
upstream reports, installed package database records, and smoke-test outputs.

## Evidence and limits

All new local logs are under the ignored directory
`validation/logs/portage-release-20261024/`. The qualifying Gentoo run is
`gentoo-2/`; its atomic `result.json` says `passed` and identifies:

- Image: `sha256:dc9477b4ceb81d718eabe1c7325941c88641a52e5f7368968e6d741fb93f12b7`.
- Candidate Git commit: `0db493b7a9d2af4ced90f02239b74cfd741563c1`.
- Candidate tree: `99cba6221459ca9504a21feda14ac9b4ce839c81`.
- Installed binary SHA-256:
  `ad8e54227f5942c2d12ad0409fff17c27b816564b17e033e94aa565dbbc76aa2`.

The candidate commit is the harness's local source snapshot, not an upstream
commit. The executable reports `omktr 0db493b7a9d2+dirty`; Portage-generated
build metadata exists beside the fetched source. The recorded tree and binary
hash identify what was qualified.

Earlier exploratory attempts exposed retained Portage completion stamps and
a sandbox detector matching quoted negative-test commands. Both were fixed;
regressions cover the detector, and the final fresh builds demonstrate the
clean-phase fix. Real archived namespace failures still trigger the detector.
Earlier fixture-only evidence remains under
`validation/logs/portage-20261024-e44b5e6f/full-sandbox/`.

These results qualify the listed native Gentoo configuration and packages.
Cross-compilation, Prefix, arbitrary third-party packages, and a CI run of this
uncommitted candidate are not claimed. Disposable containers were used for
installation; the host's Portage configuration was not modified.
