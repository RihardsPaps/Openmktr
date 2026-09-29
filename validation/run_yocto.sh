#!/usr/bin/env bash
set -eo pipefail

campaign=/root/universal-tool-campaign-20260927
source "$campaign/sources/poky-5.0.10/oe-init-build-env" \
  "$campaign/builds/yocto-5.0.10" >/dev/null
exec bitbake "$@"
