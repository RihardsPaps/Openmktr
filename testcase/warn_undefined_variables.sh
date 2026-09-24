#!/bin/sh

# Undefined-variable warnings are diagnostics only: expansion still produces
# the normal empty value, and defined empty variables are not warned about.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
DEFINED_EMPTY =

all:
	@printf '<$(UNDEFINED_VALUE)>$(DEFINED_EMPTY)' > output
EOF

(cd "$tmp" && "$mk" -s -f Makefile all >/tmp/kati-undefined-no-warning.out 2>&1)
[ ! -s /tmp/kati-undefined-no-warning.out ] || {
  echo "warning emitted without --warn-undefined-variables" >&2
  exit 1
}
[ "$(cat "$tmp/output")" = '<>' ] || exit 1

rm -f "$tmp/output"
(cd "$tmp" && "$mk" --warn-undefined-variables -s -f Makefile all \
  >/tmp/kati-undefined-warning.out 2>&1)
grep -q "undefined variable 'UNDEFINED_VALUE'" \
  /tmp/kati-undefined-warning.out || {
  echo "undefined-variable warning was not emitted" >&2
  exit 1
}
[ "$(cat "$tmp/output")" = '<>' ] || exit 1
