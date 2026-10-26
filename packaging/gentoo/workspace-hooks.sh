# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
# Optional EAPI 8 user hooks. Source only for explicitly selected packages.

_omktr_workspace_paths() {
	local action=$1 paths=${T}/openmktr-workspace.paths
	: "${OMKTR_WORKSPACE_ROOT:=/var/cache/openmktr/workspaces}"
	addwrite "${OMKTR_WORKSPACE_ROOT}"
	B=${B:-${S}}
	export S B EAPI CATEGORY PN SLOT CHOST CBUILD WORKDIR PORTAGE_BUILDDIR
	export DEPEND BDEPEND USE EPREFIX
	/usr/bin/omktr workspace "${action}" --root "${OMKTR_WORKSPACE_ROOT}" \
		> "${paths}" || die "Openmktr persistent workspace ${action} failed"
	local -a workspace_paths
	mapfile -t workspace_paths < "${paths}"
	[[ ${#workspace_paths[@]} == 2 ]] || die "Invalid Openmktr workspace paths"
	S=${workspace_paths[0]}
	B=${workspace_paths[1]}
	export S B
	export OMKTR_WORKSPACE_SOURCE=${S} OMKTR_WORKSPACE_BUILD=${B}
}

post_src_prepare() {
	_omktr_workspace_paths portage-prepare
}

_omktr_workspace_enter() {
	_omktr_workspace_paths portage-enter
	cd "${S}" || die "Cannot enter restored source workspace"
}

pre_src_configure() { _omktr_workspace_enter; }
pre_src_compile() { _omktr_workspace_enter; }
pre_src_test() { _omktr_workspace_enter; }
pre_src_install() { _omktr_workspace_enter; }
post_src_install() { _omktr_workspace_paths portage-enter; }
