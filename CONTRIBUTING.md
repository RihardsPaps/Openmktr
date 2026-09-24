# Contributing

Issues and pull requests for this GNU-free Kati fork belong in [this repository](https://github.com/RihardsPaps/UNIVERSAL_GNUMAKE_TO_NINJA_TOOL). Rihards Paps and Haralds Paps maintain this fork. Google's contributor license agreement applies to Google's projects, not to contributions submitted here.

Please keep the project-controlled build, test, CI, and runtime paths free of GNU software dependencies. Make syntax compatibility is part of `ckati`'s purpose; recipes in user-supplied Makefiles are outside that requirement. Preserve upstream copyright and license notices when modifying existing source files.

Before submitting a pull request, build and run the tests in the Chimera container:

```sh
docker build -t kati-test .
docker run --rm kati-test
```

For changes to Makefile behavior, add or update a fixture in `testcase/` and review the regression snapshots. `python tests/regression.py --record` rewrites expected results; include only intentional changes. For performance changes, compare medians and peak memory with `python tests/bench.py ./ckati --baseline /path/to/baseline/ckati` using binaries built with the same toolchain.

Keep pull requests focused and describe behavior changes, test results, and any compatibility or performance tradeoffs. Add people to [CONTRIBUTORS](CONTRIBUTORS) for contributions to this fork; [AUTHORS](AUTHORS) retains the upstream copyright author record separately.
