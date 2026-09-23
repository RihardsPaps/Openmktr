#!/bin/sh

# GNU make's dry-run spellings must plan and print recipes without executing
# them. This is a command-line compatibility feature, not a build-system
# specific rule.
set -u

mk=$(realpath "$1")

for option in -n --just-print --dry-run; do
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' RETURN
  cat >"$tmp/Makefile" <<'EOF'
all: output

output:
	@printf built > output
EOF

  if ! output=$(cd "$tmp" && "$mk" "$option" -f Makefile all 2>&1); then
    echo "dry-run option failed: $option" >&2
    exit 1
  fi
  printf '%s\n' "$output" | grep -q 'printf built > output' || {
    echo "recipe was not printed for $option" >&2
    exit 1
  }
  [ ! -e "$tmp/output" ] || {
    echo "dry-run executed recipe for $option" >&2
    exit 1
  }
done
