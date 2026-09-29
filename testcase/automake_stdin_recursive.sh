#!/bin/sh

# Automake bootstraps dependency files from a filtered stdin Makefile, then
# delegates all to all-recursive and builds each subdirectory.
set -eu
mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/child"
cat >"$tmp/Makefile" <<'EOF'
.PHONY: all all-recursive
all:
	@$(MAKE) all-recursive
all-recursive:
	@for subdir in child; do (cd $$subdir && $(MAKE) all) || exit 1; done
EOF
cat >"$tmp/child/Makefile" <<'EOF'
-include dependency.mk # am--include-marker
.PHONY: all am--depfiles
am--depfiles:
	@printf 'VALUE := bootstrapped\n' > dependency.mk
all: result
result: dependency.mk
	@printf '%s\n' '$(VALUE)' > result
EOF
(
  cd "$tmp/child"
  # A file literally named '-' must not replace standard input.
  printf '$(error read a file named dash)\n' > ./-
  sed '/# am--include-marker/d' Makefile | "$mk" -f - am--depfiles
)
(cd "$tmp" && "$mk" all)
test "$(cat "$tmp/child/result")" = bootstrapped
rm "$tmp/child/result"
(cd "$tmp" && "$mk" --ninja --regen -f Makefile all >/dev/null 2>&1)
(cd "$tmp" && sh ./ninja.sh -j12 all >/dev/null)
test "$(cat "$tmp/child/result")" = bootstrapped
rm "$tmp/child/result"
(cd "$tmp" && sh ./ninja.sh -j12 all >/dev/null)
test "$(cat "$tmp/child/result")" = bootstrapped

# A generated include causes exec-based reparsing. Keep the piped source.
cat >"$tmp/restart.mk" <<'EOF'
include generated.mk
.PHONY: all
all:
	@test '$(READY)' = yes
generated.mk:
	@printf 'READY := yes\n' > $@
EOF
(cd "$tmp" && cat restart.mk | "$mk" -f - all)

# Read beyond a single buffer and accept regular-file redirection as well.
awk 'BEGIN { for (i = 0; i < 9000; ++i) print "# padding" }' > "$tmp/large.mk"
cat "$tmp/restart.mk" >> "$tmp/large.mk"
(cd "$tmp" && "$mk" -f - all < large.mk)
(cd "$tmp" && cat large.mk | "$mk" -f - all)

# Changed stdin must invalidate --regen even if a file named '-' exists.
printf '$(error read a file named dash)\n' > "$tmp/-"
for value in first second; do
  printf '.PHONY: all\nall:\n\t@printf "%s" > stdin-result\n' "$value" > "$tmp/input.mk"
  (cd "$tmp" && cat input.mk | "$mk" --ninja --regen -f - all >/dev/null 2>&1)
  (cd "$tmp" && sh ./ninja.sh -j2 all >/dev/null)
  test "$(cat "$tmp/stdin-result")" = "$value"
done

# Only the -f option treats '-' specially; an include reads that pathname.
printf 'FROM_DASH := included\n' > "$tmp/-"
cat >"$tmp/include-dash.mk" <<'EOF'
include -
.PHONY: all
all:
	@test '$(FROM_DASH)' = included
EOF
(cd "$tmp" && cat include-dash.mk | "$mk" -f - all >/dev/null)
