#!/bin/sh
# Keep prebuilt Info files from a pristine release archive. Automake's
# regeneration recipe backs up and deletes them if MAKEINFO --version succeeds.
if [ "${1-}" = --version ]; then
  exit 1
fi
exit 0
