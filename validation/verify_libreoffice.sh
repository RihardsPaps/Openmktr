#!/usr/bin/env bash
set -euo pipefail

campaign=/root/universal-tool-campaign-20260927
build="$campaign/builds/libreoffice-25.2.7.2"
smoke="$campaign/builds/libreoffice-smoke"
mkdir -p "$smoke"
printf 'Universal Tool LibreOffice smoke test\n' > "$smoke/input.txt"
rm -f "$smoke/input.pdf"
timeout 120 "$build/instdir/program/soffice" \
    -env:UserInstallation="file://$smoke/profile" \
    --headless --convert-to pdf --outdir "$smoke" "$smoke/input.txt"
test -s "$smoke/input.pdf"
test "$(head -c 5 "$smoke/input.pdf")" = '%PDF-'
echo 'LibreOffice generated a nonempty PDF with the expected signature.'
