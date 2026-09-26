#!/bin/sh

# A VPATH-visible parent directory satisfies the parent-directory prerequisite
# for a target even when the directory is absent from the current tree.
set -eu

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/build/lto-plugin" "$tmp/source/lto-plugin" "$tmp/source/gcc"
touch "$tmp/source/gcc/marker"
cat >"$tmp/build/lto-plugin/Makefile" <<'EOF'
VPATH = ../../source/lto-plugin

../gcc/marker:
	@:
EOF

(cd "$tmp/build/lto-plugin" && "$mk" -f Makefile ../gcc/marker >/dev/null)

# Recipes such as Automake's .deps/.dirstamp create the output directory
# themselves. The inferred parent edge must not turn an unruled directory
# into a missing-prerequisite error.
cat >"$tmp/Makefile" <<'EOF'
all: tools/.deps/.dirstamp

tools/.deps/.dirstamp:
	@mkdir -p tools/.deps
	@touch $@
EOF
(cd "$tmp" && "$mk" -f Makefile all >/dev/null)
[ -f "$tmp/tools/.deps/.dirstamp" ]
