#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Portage installs versioned LLVM binaries and exports their paths in profile.
set -e
. /etc/profile
set -u
PATH=/opt/openmktr-ninja:$PATH
export PATH
exec "$@"
