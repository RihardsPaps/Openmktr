#!/bin/bash

# GNU make keep-going mode continues independent work but does not run a
# target whose prerequisite failed.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all fail independent blocked
all: fail independent blocked

fail:
	@printf ran > fail.ran
	@false

independent:
	@printf ran > independent.ran

blocked: fail
	@printf ran > blocked.ran
EOF

if (cd "$tmp" && "$mk" -k -j3 -f Makefile all >/dev/null 2>&1); then
  echo "keep-going reported success after a failed recipe" >&2
  exit 1
fi
[ -e "$tmp/fail.ran" ] || exit 1
[ -e "$tmp/independent.ran" ] || {
  echo "keep-going did not continue an independent target" >&2
  exit 1
}
[ ! -e "$tmp/blocked.ran" ] || {
  echo "keep-going ran a target with a failed prerequisite" >&2
  exit 1
}

rm -f "$tmp/fail.ran" "$tmp/independent.ran"
if (cd "$tmp" && "$mk" --keep-going -j3 -f Makefile all >/dev/null 2>&1); then
  echo "--keep-going reported success after a failed recipe" >&2
  exit 1
fi
[ -e "$tmp/independent.ran" ] || exit 1
