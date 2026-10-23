# Optional compatibility environments

Run **Optional external compatibility and benchmarks** with Actions' manual
workflow dispatch on the branch to test. It supplements the required Chimera
correctness, sanitizer, and external incremental comparison gates. It does not
relax them or claim compatibility with every third-party Makefile.
Pull requests that change this workflow, its environment files, the jobserver
test, or the benchmark harness also run the lanes to validate those changes.

The Alpine 3.23 lane builds with Clang, LLD, musl, libc++, and BusyBox. Alpine's
LLVM packages also pull in GNU toolchain components as distribution dependencies.
The Gentoo stage3 lane is an external glibc environment, with its own system
utilities and package build dependencies. Both are external comparison images;
neither changes the portable Dockerfile or Openmktr's runtime requirements.
Gentoo uses binary packages when available; source package fallback can make its
initial build slow. Tags and package feeds can move; retain Docker build logs
and printed tool versions with results.

Both environments run unit, version, correctness, Make compatibility, include
dump, and explicit Ninja >= 1.13 jobserver checks. Environment-specific snapshot
output is left to the reference Chimera gate. The jobserver check supplies a
GNU-protocol FIFO using Python, verifies a plain Ninja client, then runs generated
launchers with recursive Openmktr children, with and without explicit `-j1`.
It checks bounded concurrency, completion, failure propagation, and token return
under timeouts. GNU and Openmktr FIFOs are distinct; this does not claim that
Openmktr adopts an enclosing GNU Make job budget.

Ninja's [jobserver documentation](https://ninja-build.org/manual.html#_gnu_jobserver_support)
describes the no-`-j`, `MAKEFLAGS`, and FIFO conditions. A version below 1.13
fails this optional check rather than silently omitting coverage.

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
