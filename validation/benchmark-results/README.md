# Aggressive graph benchmark results

Paired single-sample `-j8` measurements against GNU Make on Gentoo Linux x86_64.
The candidate used `--graph_optimizations=aggressive --use_ninja_phony_output`
and the Ninja `build` entrypoint. Each JSON report contains the environment,
executable hashes, archive hashes, output checks, and per-scenario timings. The
adjacent `*-commands.jsonl` files preserve the full command and resource-usage
journals.

| Project | Scenario | GNU Make (s) | Openmktr (s) | Change |
|---|---|---:|---:|---:|
| Linux 6.12 | clean | 354.66 | 354.05 | 0.17% faster |
| Linux 6.12 | no-op | 3.43 | 5.50 | 60.13% slower |
| Linux 6.12 | source touch | 17.29 | 20.16 | 16.55% slower |
| Linux 6.12 | missing output | 17.42 | 3.63 | 79.19% faster |
| Linux 6.12 | graph touch | 3.33 | 0.80 | 76.13% faster |
| FFmpeg 7.1.1 | clean | 86.56 | 88.24 | 1.93% slower |
| FFmpeg 7.1.1 | no-op | 0.23 | 0.08 | 65.16% faster |
| FFmpeg 7.1.1 | source touch | 1.00 | 0.86 | 14.34% faster |
| FFmpeg 7.1.1 | missing output | 0.99 | 0.86 | 13.36% faster |
| FFmpeg 7.1.1 | graph touch | 0.23 | 0.08 | 65.76% faster |

The touched-source and missing-output scenarios verify that the expected object
is rebuilt, unrelated objects are not rebuilt, and final artifacts match the
clean build. The clean and incremental numbers are one paired sample each;
they are useful diagnostics, not a multi-sample performance qualification.

## Remaining bottlenecks

- Kbuild source changes trigger the conservative root recursive fallback. This
  is needed because the imported graph hides Kbuild's `.cmd` object dependency
  edges. The fallback reparses many recursive frontends and makes this case
  slower than GNU Make.
- The source freshness proof records a large source set. Validating those file
  identities adds work to the first Kbuild no-op invocation. The steady-state
  imported graph was faster in local warmed runs, but the paired benchmark's
  first no-op cell still regressed.
- Linux clean is at parity, while FFmpeg clean remains about 2% slower.

Likely next improvements are to map changed source/header paths to the owning
Kbuild `.cmd` outputs and invalidate only those imported subgraphs, and to batch
or parallelize source identity checks without weakening the source-change
correctness proof. The source-to-object mapping must keep header changes and
generated prerequisites correct; a prior narrower fallback skipped
`init/main.o` and was not retained.

## Reproduction

The reports were produced with `validation/benchmark_ninja.py`, GNU Make as the
baseline, Linux 6.12 and FFmpeg 7.1.1 release archives, jobs 8, one sample, no
warmups, and the aggressive candidate options listed above. The exact
invocations and resource-usage measurements are in the adjacent command logs.
