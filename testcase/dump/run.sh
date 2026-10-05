#!/bin/sh -eu
# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original notices below remain applicable.


KATI="${KATI:=$PWD/omktr}"
export KATI

for TESTCASE in testcase/dump/*; do
  if [ ! -d "$TESTCASE" ]; then
    continue
  fi

  echo "Running $TESTCASE..."
  "$TESTCASE/test.sh"
done
