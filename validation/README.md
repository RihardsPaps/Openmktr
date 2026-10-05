# Validate external Makefile builds

Use this directory to investigate real builds and inspect recorded compatibility
evidence. [The results matrix](PROGRESS.md) names the pinned releases,
configurations, and limits. It records 41 selected passes, one user-requested
AOSP skip, and an unresolved NetBSD dialect gap; complete ordered campaign
acceptance has not passed.

## Choose the right checks

For a portable pull-request gate, follow [the contributor
guide](../CONTRIBUTING.md#run-the-portable-checks).
The campaign helpers target Ubuntu 24.04 in WSL and a pinned Chimera chroot.
Many contain absolute paths to `/root/universal-tool-campaign-20260927` and
this checkout under `/mnt/c`. They are debugging and evidence tools for that
builder, not a portable replacement for CI.

The WSL campaign host and third-party build recipes can use GNU tools. The
project's GNU-free build and test guarantee applies to the supported Chimera
path, as described in [the README](../README.md).

## Preserve evidence

Keep release source trees separate from disposable build trees. Do not patch
third-party sources to obtain a pass. Record the release or source revision,
archive digest where applicable, configuration, Openmktr revision or binary digest,
command, exit status, and artifact verification. Include repeat builds and
original-source integrity checks before marking a project as passed.

[`phase.py`](phase.py) writes command output to `REPORT_DIR/logs/` and appends
a timestamp, working directory, command, exit status, elapsed time, and log
filename to `REPORT_DIR/PROGRESS.md`. Its default report directory is `/report`.
For example, on Linux from the repository root:

```sh
python3 validation/phase.py --name kati-version --cwd "$PWD" \
  --report "$PWD/validation" -- "$PWD/omktr" --version
```

This records a version probe only. It is not a compatibility pass. Full campaign
logs are ignored under `validation/logs/`; the checked-in
[historical ledger](history.log) preserves their recorded names and phase results.

## Check the recorded WSL candidate

Only on the builder with the campaign's paths and chroot prepared, run these
commands from the repository root:

```sh
python3 validation/check_chimera_format.py
bash validation/validate_candidate_chimera.sh
```

The format helper checks changed C++ files with the chroot's clang-format.
The candidate helper copies selected checkout files into the existing Chimera
workspace, builds Openmktr, and runs C++ units, version generation, correctness,
Make compatibility, snapshots, and the dump smoke test. It uses the existing
chroot state; CI supplies the portable clean-image and sanitizer checks.

## Diagnose recursive Ninja failures

A Ninja failure at an `all-recursive` edge identifies the failed recipe, but
its child-process output is needed to identify the cause. Recursive shell loops
remain recipes; child Openmktr processes evaluate their subdirectory Makefiles at
execution time.

For a failing build, save the executable revision and complete launcher output:

```sh
omktr --version
sh ninja.sh -j12 all > tar-build.log 2>&1
```

Check the configure exit status, generated subdirectory Makefiles, and any
overrides to `MAKE`, `MAKEFLAGS`, or `MAKEOVERRIDES`. A missing `po/Makefile`
can produce this child error in GNU tar 1.35:

```text
Making all in po
*** No targets specified and no makefile found.
*** [all-recursive] Error 1
```

That controlled example does not establish what caused the reporter's failure.
The original [issue
#20](https://github.com/RihardsPaps/Openmktr/issues/20)
failure remained unreproduced with a completely configured tree. The confirmed
Automake stdin defects were addressed in PR #21: `-f -` reads standard input,
`include -` still reads a literal dash filename, restarts preserve the input,
and changed stdin cannot reuse a stale Ninja graph. The
[Automake fixture](../testcase/automake_stdin_recursive.sh) covers those paths.

## Reproduce the GNU tar check

On Linux with Python 3.12 or newer, Clang, Ninja, and tar's configure
prerequisites, provide a tar 1.35 release archive and the Openmktr executable:

```sh
python3 validation/reproduce_issue20.py /path/to/tar-1.35.tar.gz /path/to/omktr
```

Replace both paths with existing files. The helper creates disposable in-source
and out-of-source trees, clears inherited Make and Openmktr jobserver state, and
retains combined command output, exit statuses, Openmktr revision, and archive hash.
It checks initial and repeated builds, relinking, changed-source rebuilds,
regeneration, and recovery from a deliberately removed `po/Makefile`. It restores
that file before the recovery check and does not download sources or modify
existing release trees.

The recorded checks at revision `c2ff318561675caa4c82d7916dcc81254d47972e`
passed both layouts in the pinned Chimera chroot. The archive SHA-256 was
`14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e`.
See [the archived investigation](history.log) for historical suite counts,
optimized-Python checks, and the distinction between confirmed fixes and the
unreproduced report. These results do not justify claiming a new runtime fix
or closing the original report without evidence of its cause.
