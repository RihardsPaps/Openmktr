# SPDX-License-Identifier: MPL-2.0
# Merge this snippet into /etc/portage/bashrc. package.env accepts settings,
# while phase functions must be loaded through Portage's shell hook.
if [[ -n ${OMKTR_WORKSPACE_ROOT} ]]; then
	source /usr/share/openmktr/workspace-hooks.sh
fi
