# Build compatibility campaign

[`PROGRESS.md`](PROGRESS.md) is the live record for the 43 requested project
rows. It lists pinned versions, configurations, fixes, phase results, artifact
checks, source-integrity checks, and the current acceptance boundary. Consult
its project matrix before treating a project as supported.

The scripts in this directory reproduce checks on the campaign's Ubuntu 24.04
WSL builder. They intentionally refer to its source and build trees under
`/root/universal-tool-campaign-20260927` and to this workspace under `/mnt/c`;
they are evidence and debugging aids, not a portable CI harness. `phase.py`
appends command, exit status, elapsed time, and a log filename to the single
progress record. Full logs stay in the ignored `validation/logs/` directory.
Release source trees are separate from disposable build trees, and the campaign
does not patch third-party project sources.

For a local compatibility check in the recorded environment:

```sh
python3 validation/check_chimera_format.py
bash validation/validate_candidate_chimera.sh
```

The latter builds ckati in the pinned Chimera chroot and runs the C++ unit
tests, version-generation tests, focused correctness tests, canonical regression
scenarios, and dump smoke test. The repository's GitHub Actions workflow remains
the portable PR gate, including its sanitizer run.

At the latest checkpoint, 41 selected configurations passed. AOSP was skipped
at the user's request. NetBSD 10.1 still requires BSD Make dialect support, so
the requested complete ordered campaign has not passed.
