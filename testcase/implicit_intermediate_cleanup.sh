#!/bin/bash

# GNU make removes files created only as links in an implicit-rule chain.
# Verify that direct Kati and generated Ninja preserve that generic behavior,
# while .SECONDARY protects the same file.
set -u

mk=$(realpath "$1")

run_case() {
  local mode=$1
  local secondary=$2
  local tmp
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' RETURN

  cat >"$tmp/Makefile" <<EOF
$secondary

all: input.out

%.out: %.mid
	@cat \$< > \$@

%.mid: %.src
	@printf 'generated:'; cat \$< > \$@

input.src:
	@printf source > \$@
EOF

  if [ "$mode" = direct ]; then
    (cd "$tmp" && "$mk" -f Makefile all >/dev/null)
  else
    (cd "$tmp" && "$mk" --ninja --regen -f Makefile >/dev/null 2>&1)
    (cd "$tmp" && ./ninja.sh -j2 >/dev/null)
  fi

  [ -f "$tmp/input.out" ] || {
    echo "implicit final output missing ($mode)" >&2
    return 1
  }
  if [ -n "$secondary" ]; then
    [ -f "$tmp/input.mid" ] || {
      echo ".SECONDARY did not preserve implicit intermediate ($mode)" >&2
      return 1
    }
  else
    [ ! -e "$tmp/input.mid" ] || {
      echo "implicit intermediate output was not cleaned ($mode)" >&2
      return 1
    }
  fi
}

run_case direct ''
run_case direct '.SECONDARY: input.mid'
run_case direct '.SECONDARY:'
run_case ninja ''
run_case ninja '.SECONDARY: input.mid'
run_case ninja '.SECONDARY:'
