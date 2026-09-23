#!/bin/sh
set -eu

kati=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
all: generated

generated: child-dir
	@printf '%s\n' generated > $@

child-dir:
	@mkdir -p $@
	@false
EOF

(cd "$tmp" && "$kati" --ninja --regen -f Makefile all >/dev/null 2>&1)
if (cd "$tmp" && ./ninja.sh -j2 all >/dev/null 2>&1); then
  echo "directory recipe failure was swallowed" >&2
  exit 1
fi
