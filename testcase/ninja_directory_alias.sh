#!/bin/sh
set -e

mkdir -p doc
printf 'ok\n' > doc/source

cat > Makefile <<'EOF'
all: doc
doc: documentation
documentation: doc/generated
doc/generated: doc/tool | doc/
	@cp doc/tool doc/generated
doc/tool: doc/source | doc/
	@cp doc/source doc/tool
EOF

"$@" all > /dev/null
sh ./ninja.sh -j24 all > /dev/null
cat doc/generated
