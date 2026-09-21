#!/bin/bash

# GNU make question mode reports whether a target needs rebuilding without
# executing recipes, including recipes of phony targets.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: phony
all: output phony

phony:
	@printf ran > phony.ran

output:
	@printf built > output
EOF

if (cd "$tmp" && "$mk" -q -f Makefile all >/dev/null 2>&1); then
  echo "question mode missed a missing target" >&2
  exit 1
fi
[ ! -e "$tmp/output" ] || {
  echo "question mode ran a recipe" >&2
  exit 1
}

(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ "$(cat "$tmp/output")" = built ] || exit 1
rm -f "$tmp/phony.ran"

if (cd "$tmp" && "$mk" --question -f Makefile all >/dev/null 2>&1); then
  echo "question mode ignored the phony target" >&2
  exit 1
fi
[ ! -e "$tmp/phony.ran" ] || {
  echo "question mode ran a phony recipe" >&2
  exit 1
}

rm -f "$tmp/phony.ran"
cat >"$tmp/Makefile" <<'EOF'
all: output

output:
	@printf built > output
EOF

(cd "$tmp" && "$mk" -q -f Makefile all >/dev/null)
