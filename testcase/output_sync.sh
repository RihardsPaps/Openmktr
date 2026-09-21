#!/bin/bash

# Output synchronization is generic executor behavior: target mode keeps a
# recipe's captured output together even when independent recipes run in
# parallel, while all GNU Make mode spellings are accepted.
set -u

mk=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/Makefile" <<'EOF'
.PHONY: all a b
all: a b

a:
	@printf A1; sleep 0.05; printf A2; : > a

b:
	@printf B1; sleep 0.05; printf B2; : > b
EOF

for mode in target line none recurse; do
  rm -f "$tmp/a" "$tmp/b" "$tmp/output"
  (cd "$tmp" && "$mk" -s -j2 --output-sync="$mode" -f Makefile all > output)
  compact=$(tr -d '\n' <"$tmp/output")
  if [ "$mode" = target ] || [ "$mode" = recurse ]; then
    case "$compact" in
      A1A2B1B2|B1B2A1A2) ;;
      *)
        echo "output-sync=$mode interleaved target output" >&2
        exit 1
        ;;
    esac
  else
    case "$compact" in
      *A1*A2*B1*B2*|*B1*B2*A1*A2*) ;;
      *)
        echo "output-sync=$mode lost recipe output" >&2
        exit 1
        ;;
    esac
  fi
done

rm -f "$tmp/a" "$tmp/b" "$tmp/output"
(cd "$tmp" && "$mk" -s -j2 --output-sync -f Makefile all > output)
[ -s "$tmp/output" ] || {
  echo "bare --output-sync produced no output" >&2
  exit 1
}
