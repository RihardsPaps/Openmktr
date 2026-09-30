# Agent guidance

## Project context

This repository is a C++17 fork of Google Kati. `ckati` reads Makefiles,
executes targets directly, or generates Ninja graphs. Linux with musl is the
supported platform; other platforms are best effort. Read `README.md` before
changing build or runtime behavior.

- Keep the project-controlled build, tests, and runtime GNU-free: Clang/LLVM,
  musl, libc++, Ninja, Python, and POSIX shell tools. Do not introduce a
  dependency on GNU Make, GCC, glibc, Bash, or GNU utilities. Commands in
  user-supplied Makefiles are outside this guarantee.
- Preserve upstream copyright and license notices. Follow `.clang-format`
  (Chromium style) for C++ changes, and keep fixes focused on the reported issue.
- Follow `docs/LICENSING.md`: original fork contributions use PolyForm Perimeter
  1.0.1; upstream Apache material retains its terms. Add or update prominent
  fork change notices in modified upstream files and preserve fixture semantics.
- Parsing and evaluation live in `src/parser.cc`, `src/expr.cc`, `src/eval.cc`,
  `src/stmt.cc`, and `src/func.cc`; dependency resolution in `src/dep.cc`;
  direct execution in `src/command.cc` and `src/exec.cc`; Ninja generation and
  regeneration in `src/ninja.cc` and `src/regen.cc`.
- Check behavior in both direct and Ninja modes when changing shared semantics.
  Include recursive builds, exports, regeneration, or parallel execution when
  those paths are affected.
- Run conversion examples in a disposable directory, never the repository
  root: conversion can overwrite the project's checked-in `build.ninja`.
- `validation/` contains environment-specific compatibility campaign tools.
  Read `validation/README.md` and `validation/PROGRESS.md` before using them or
  claiming compatibility. They are not a portable substitute for CI; do not
  patch third-party release source trees to make campaign checks pass.

## GitHub access

- Use the installed GitHub integration for repository metadata, issues,
  comments, pull requests, reviews, and CI results. Never use the user's
  browser or browser automation for GitHub work.
- Local Git commands are appropriate for inspecting and editing the checkout,
  fetching, updating branches, committing, and pushing. Use the GitHub
  integration for GitHub API operations rather than `gh` or ad hoc HTTP calls.
- If a required integration operation is unavailable, explain the blocker;
  do not fall back to the user's browser.

## Required workflow for a reported issue

When the user asks to fix a reported GitHub issue, carry the work through the
following steps without stopping after a local fix:

1. Read the issue and its comments through the GitHub integration. Identify
   the reported behavior, reproduction, and acceptance criteria.
2. Inspect local Git status and preserve existing user changes. Fetch
   `origin`, switch to local `main`, and fast-forward it to `origin/main`
   (`git fetch origin`, `git switch main`, `git pull --ff-only origin main`).
   Verify that local `main` matches the fetched remote main before branching.
   If local changes or divergent commits prevent this, resolve without
   discarding user work; ask for direction only when necessary. Never reset
   hard or force-push to bypass the problem.
3. Create a new branch from the updated local `main`, normally named
   `codex/issue-<number>-<short-description>`. Do not implement the fix on main.
4. Reproduce the issue, implement the fix, and add a regression test that
   checks the reported failure. Run the relevant checks below and review the
   diff for unrelated changes.
5. Commit and push the branch. Create a PR targeting `main` using the GitHub
   integration. Explain the problem, resulting behavior, and validation;
   include `Fixes #<number>` and disclose any unrun checks or known limitations.
   Attach the created PR to the current Codex chat using `attach_artifact`.
6. Check the PR's CI through the integration and address failures attributable
   to the fix. If validation is blocked, report it accurately and leave the
   issue open until the fix is adequately verified.
7. After creating the PR and verifying the fix, close the issue through the
   GitHub integration with a courteous comment: thank the reporter, briefly
   explain the fix, link the PR, and summarize validation. If the PR is still
   awaiting review or merge, say so explicitly; do not claim it is released.
   A request to fix an issue authorizes this PR and issue-closing workflow;
   it does not authorize merging the PR.
8. Report the PR link, issue closure, and validation results to the user.

## Validation

The portable PR gate is `.github/workflows/cpp-ci.yml`, using the Chimera
image in `Dockerfile`. For code changes, run the supported image when available:

```sh
docker build -t kati-test .
docker run --rm kati-test
```

Inside Chimera, the standard build and tests are:

```sh
ninja -f build.ninja -j4 ckati tests
out/find_test && out/ninja_test && out/strutil_test
python tests/version_generator.py
python tests/correctness.py
python tests/make_compat_regressions.py
python tests/regression.py
sh testcase/dump/run.sh
```

- Check changed C++ files with `sh clang-format-check` in Chimera; set
  `CLANG_FORMAT_FILES` to the space-separated changed paths to limit scope.
- Add behavior fixtures in `testcase/` or extend the relevant focused Python
  suite. Use `python tests/regression.py --case pattern` for focused scenarios.
  Record changed expectations with `python tests/regression.py --record` in
  Chimera and review the snapshot diff. Never record a bug as the new expected
  behavior or add a new crash to `tests/known_crashes.json` to pass tests.
- For memory safety or undefined behavior changes, run the sanitizer build
  and suites described in `README.md` and the CI workflow.
- For performance changes, compare with `python tests/bench.py` using baseline
  and candidate binaries built with the same toolchain.
- If Docker, Chimera, or another required tool is unavailable locally, report
  what was actually checked and inspect CI through the GitHub integration.
  Never present unavailable or unrun tests as passing.
- Documentation-only edits need a diff and accuracy review; a full runtime
  test run is unnecessary unless executable behavior also changes.
