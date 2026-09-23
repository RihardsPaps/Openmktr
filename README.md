# Kati

Kati reads Makefiles, executes their targets directly, or converts them to a Ninja build graph. This repository ships the parallel C++ `ckati` implementation.

## GNU-free build

The supported build and test environment is [Chimera Linux](https://chimera-linux.org/about/): Clang/LLVM, musl, libc++, a BSD-derived userland, Python, and Ninja. No GNU Make, GCC, glibc, Bash, or GNU utility is required by the project-controlled build or test path. Commands supplied by a user's Makefile are external inputs and may use tools of the caller's choice.

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
