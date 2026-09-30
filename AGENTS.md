# Agent guidance

## Project rules

This repository is a C++17 fork of Google Kati. `ckati` reads Makefiles,
executes targets, or generates Ninja graphs. Linux with musl is supported;
other platforms are best effort. Read [README.md](README.md) before changing
build or runtime behavior and [CONTRIBUTING.md](CONTRIBUTING.md) for validation
commands and source layout.

- Keep the project-controlled build, tests, and runtime GNU-free. Use Clang/LLVM, musl,
  libc++, Ninja, Python, and POSIX shell tools. Do not introduce GNU Make, GCC, glibc,
  Bash, or GNU utilities as dependencies. Commands in user-supplied Makefiles are
  outside this guarantee.
- Follow `.clang-format` (Chromium style) for C++ and keep fixes focused on the reported
  issue.
- Follow [docs/LICENSING.md](docs/LICENSING.md). Original fork contributions use
  PolyForm Perimeter 1.0.1; upstream Apache material retains its terms. Preserve
  copyright and license notices, update prominent fork change notices in modified
  upstream files, and preserve fixture semantics.
- Parsing and evaluation live in `src/parser.cc`, `src/expr.cc`, `src/eval.cc`,
  `src/stmt.cc`, and `src/func.cc`; dependency resolution in `src/dep.cc`; direct
  execution in `src/command.cc` and `src/exec.cc`; Ninja generation and regeneration in
  `src/ninja.cc` and `src/regen.cc`.
- Test direct and Ninja modes when changing shared semantics. Include recursive builds,
  exports, regeneration, and parallel execution when affected.
- Run conversion examples in disposable directories. Conversion at the repository root
  can overwrite the checked-in `build.ninja`.
- Read [validation/README.md](validation/README.md) and
  [validation/PROGRESS.md](validation/PROGRESS.md) before using campaign tools or
  claiming compatibility. They target a specific environment and do not replace CI. Do
  not patch third-party release sources to make checks pass.

## GitHub access

Use the installed GitHub integration for repository metadata, issues, comments,
pull requests, reviews, and CI results. Never use the user's browser, browser
automation, `gh`, or ad hoc HTTP calls for GitHub API operations.

Use local Git for checkout inspection, edits, fetching, branch updates,
commits, and pushes. If a required integration operation is unavailable,
explain the blocker; do not fall back to the browser.

## Complete a reported issue

A request to fix a reported GitHub issue authorizes the following PR and
issue-closing workflow. It does not authorize merging the PR.

1. Read the issue and comments through the integration. Establish the reported behavior,
   reproduction, and acceptance criteria.
2. Inspect local Git status and preserve user changes. Run `git fetch origin`, `git
   switch main`, and `git pull --ff-only origin main`. Verify that local `main` matches
   fetched `origin/main` before branching. Resolve blocking changes or divergent commits
   without discarding work; ask for direction only when necessary. Never reset hard or
   force-push to bypass a problem.
3. Branch from updated local `main`, normally as
   `codex/issue-<number>-<short-description>`. Do not implement the fix on main.
4. Reproduce the failure, implement the fix, and add a regression test for it. Run the
   relevant validation below and review the diff for unrelated changes.
5. Commit and push. Create a PR targeting `main` through the integration, explaining the
   problem, resulting behavior, and validation. Include `Fixes #<number>` and disclose
   unrun checks and known limits. Attach the PR to this chat with `attach_artifact`.
6. Inspect PR CI through the integration and fix failures attributable to the change. If
   validation is blocked, report it accurately and leave the issue open until the fix is
   adequately verified.
7. After creating the PR and verifying the fix, close the issue through the integration
   with a courteous comment. Thank the reporter, explain the fix, link the PR, and
   summarize validation. Explicitly say when review or merge is pending; do not claim
   the fix is released.
8. Report the PR link, issue closure, and validation to the user.

## Validate changes

The portable PR gate is [.github/workflows/cpp-ci.yml](.github/workflows/cpp-ci.yml),
using the Chimera image in [Dockerfile](Dockerfile). For code changes, run the
supported image when available:

```sh
docker build -t kati-test .
docker run --rm kati-test
```

Inside Chimera, run:

```sh
ninja -f build.ninja -j4 ckati tests
out/find_test && out/ninja_test && out/strutil_test
python tests/version_generator.py
python tests/correctness.py
python tests/make_compat_regressions.py
python tests/regression.py
sh testcase/dump/run.sh
```

- Check changed C++ files with `sh clang-format-check` in Chimera. Set
  `CLANG_FORMAT_FILES` to the space-separated changed paths to limit scope.
- Add fixtures in `testcase/` or extend a focused Python suite. Select matching
  scenarios with `python tests/regression.py --case pattern`.
- Record changed expectations with `python tests/regression.py --record` in Chimera and
  review the snapshot diff. Never record a bug as expected behavior or add a new crash
  to `tests/known_crashes.json` to pass tests.
- For memory safety or undefined behavior changes, run the [sanitizer
  checks](CONTRIBUTING.md#check-memory-safety) and suites specified by CI.
- For performance changes, compare baseline and candidate binaries built with the same
  toolchain using `python tests/bench.py`.
- If Docker, Chimera, or a required tool is unavailable, report what was actually
  checked and inspect CI through the integration. Never present unrun checks as passing.
- Documentation-only changes require a diff and accuracy review; a full runtime test run
  is unnecessary unless executable behavior changes.
