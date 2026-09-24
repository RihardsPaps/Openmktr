# GNU-free Kati

This repository maintains a GNU-free C++ fork of [Google's Kati](https://github.com/google/kati). `ckati` reads Makefiles and either executes targets directly, including parallel builds, or converts them to a Ninja build graph. Make syntax compatibility does not require GNU Make at build or run time.

The supported platform is Linux with musl. Other platforms are best effort.

## GNU-free build

The build and test environment is [Chimera Linux](https://chimera-linux.org/about/): Clang/LLVM, musl, libc++, a BSD-derived userland, Python, and Ninja. The project-controlled build, test, CI, and runtime paths require no GNU Make, GCC, glibc, Bash, or GNU utilities. Commands supplied by a user's Makefile are external inputs and may use tools of the caller's choice.

Build and test in the container:

```sh
docker build -t kati-test .
docker run --rm kati-test
```

In an existing Chimera environment with `clang`, `ninja`, and `python` installed:

```sh
ninja -f build.ninja -j4 ckati tests
out/find_test && out/ninja_test && out/strutil_test
python tests/regression.py
sh testcase/dump/run.sh
```

## Usage

Run a target directly:

```sh
./ckati -f Makefile -j4 all
```

Generate and run a Ninja graph:

```sh
./ckati --ninja -f Makefile all
./ninja.sh -j4
```

The checked-in regression snapshots cover direct execution and Ninja generation without a GNU Make reference binary. To inspect or update them after an intentional behavior change, run `python tests/regression.py --record` and review the diff. For repeatable timing and peak-memory measurements, run `python tests/bench.py ./ckati`; pass `--baseline /path/to/previous/ckati` to compare binaries built with the same toolchain.

## Project and attribution

This is an independently maintained fork of Google Kati. The original source project and its contributors are credited in [AUTHORS](AUTHORS) and [CONTRIBUTORS](CONTRIBUTORS); original source copyright notices and the [Apache 2.0 license](LICENSE) are retained. Rihards Paps owns this repository and, with contributor Haralds Paps, designed and maintains its GNU-free direction. See [INTERNALS.md](INTERNALS.md) for the current architecture and [CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidance.
