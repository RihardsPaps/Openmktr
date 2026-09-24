# Kati internals

This describes the C++ `ckati` implementation maintained in this repository. It accepts Makefile syntax for compatibility. The supported executable runs directly or emits a Ninja graph; both modes share the same parser, evaluator, and dependency graph.

## Processing a Makefile

1. `src/parser.cc` parses statements and `src/expr.cc` parses expressions. `src/file_cache.cc` reuses parsed input when a Makefile is included repeatedly.
2. `src/eval.cc`, `src/stmt.cc`, and `src/func.cc` evaluate assignments, directives, functions, and rules. Evaluation produces a variable table and rules.
3. `src/dep.cc` resolves explicit, pattern, and suffix rules into the requested target's dependency graph. It uses indexed pattern matching rather than testing every rule against every target.
4. `src/command.cc` expands recipes with automatic and target-specific variables. `src/exec.cc` executes them directly, while `src/ninja.cc` writes Ninja rules.

The Makefile language has context-dependent parsing and expansion. In particular, a line can be a rule or an assignment depending on expansion, and recipe expansion happens after graph construction. Changes to these phases need direct-execution and Ninja-generation regression coverage.

## Direct execution

`src/exec.cc` walks the graph, checks target timestamps, and runs stale recipes. Independent targets can run concurrently with `-j`; the executor tracks in-progress nodes and reports dependency cycles. Recursive builds and `.NOTPARALLEL` affect scheduling. Direct execution remains a supported mode and is exercised by the snapshot suite.

## Ninja generation and regeneration

`src/ninja.cc` converts the graph and expanded recipes into Ninja syntax. Multiple Makefile recipe lines and shell metacharacters need translation and escaping. Some Make constructs have no exact Ninja equivalent, so behavior changes here need fixture coverage in both modes.

`src/regen.cc` checks whether an existing generated graph needs updating. Its inputs include Makefiles, relevant environment variables, wildcard results, and shell-derived dependencies. `--ninja --regen` can skip conversion when those inputs have not changed. The benchmark suite measures conversion and no-op regeneration separately.

## Development boundaries

The supported build uses `build.ninja` with Clang/LLVM, musl, and libc++. The project-controlled path requires no GNU software. Compatibility with GNU Make syntax is implemented in this C++ code; it does not link to or execute GNU Make. Recipes written by a Makefile author may invoke arbitrary external commands.

Use `tests/regression.py` for behavior snapshots and `tests/bench.py` for conversion, regeneration, direct execution, memory, and binary-size measurements. Build and test instructions are in [README.md](README.md).

This fork derives from [Google Kati](https://github.com/google/kati). The historical authors and contributors are retained in [AUTHORS](AUTHORS) and [CONTRIBUTORS](CONTRIBUTORS), with the original license and source notices preserved.
