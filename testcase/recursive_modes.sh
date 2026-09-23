#!/bin/sh

# Recursive execution modes must propagate through $(MAKE). In particular,
# GNU make's dry-run enters the child so its planned recipes are printed, but
# neither parent nor child recipes may modify the filesystem.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/child"

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all recurse
all: recurse
recurse:
	@$(MAKE) -C child all
EOF

cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all:
	@printf '%s' child-ran > child.out
EOF

output=$(cd "$tmp" && "$mk" -n -f Makefile all 2>&1)
printf '%s\n' "$output" | grep -F 'printf '\''%s'\'' child-ran > child.out' >/dev/null
[ ! -e "$tmp/child/child.out" ]

# -s is inherited by the child and remains visible in MAKEFLAGS.
cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all:
	@printf '%s' "$(MAKEFLAGS)" > flags.out
EOF
(cd "$tmp" && "$mk" -s -f Makefile all >/dev/null)
grep -Eq '(^|[[:space:]])-s([[:space:]]|$)' "$tmp/child/flags.out"

# -t reaches the child: an existing output is touched, but its recipe is not
# run. The marker is therefore absent.
cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all
all: child.out
child.out:
	@printf '%s' recipe-ran > marker.out
EOF
: >"$tmp/child/child.out"
(cd "$tmp" && "$mk" -t -f Makefile all >/dev/null)
[ ! -e "$tmp/child/marker.out" ]

# -k reaches the child so independent child prerequisites continue after a
# failure. The parent still reports the recursive failure, as GNU make does.
cat >"$tmp/child/Makefile" <<'EOF'
.PHONY: all bad good
all: bad good
bad:
	@false
good:
	@printf '%s' continued > continued.out
EOF
if (cd "$tmp" && "$mk" -k -f Makefile all >/dev/null 2>&1); then
  exit 1
fi
[ "$(cat "$tmp/child/continued.out")" = continued ]
