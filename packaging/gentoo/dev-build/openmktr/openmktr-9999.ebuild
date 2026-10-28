# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0

EAPI=8
LLVM_COMPAT=( {22..23} )
PYTHON_COMPAT=( python3_{14..15} )
PYTHON_REQ_USE="gui? ( tk ) tui? ( ncurses )"
NINJA=ninja-reference

inherit git-r3 llvm-r2 ninja-utils python-single-r1

DESCRIPTION="Makefile executor and Ninja graph generator with opt-in Portage integration"
HOMEPAGE="https://github.com/RihardsPaps/Openmktr"
EGIT_REPO_URI="https://github.com/RihardsPaps/Openmktr.git"
EGIT_BRANCH=main

LICENSE="MPL-2.0 Apache-2.0"
SLOT="0"
KEYWORDS=""
IUSE="test tui gui policykit"
REQUIRED_USE="${PYTHON_REQUIRED_USE}"
RESTRICT="!test? ( test )"

RDEPEND="llvm-runtimes/libcxx:=
	${PYTHON_DEPS}
	>=dev-build/ninja-1.13.2
	policykit? ( sys-auth/polkit )"
DEPEND="llvm-runtimes/libcxx:="
BDEPEND="${PYTHON_DEPS}
	>=dev-build/ninja-1.13.2
	test? ( app-arch/zstd )
	$(llvm_gen_dep '
		llvm-core/clang:${LLVM_SLOT}
		llvm-core/lld:${LLVM_SLOT}
	')"

pkg_setup() {
	llvm-r2_pkg_setup
	python-single-r1_pkg_setup
}

src_configure() {
	# Cross compilation and Prefix are not qualified by this integration.
	[[ ${CBUILD:-${CHOST}} == "${CHOST}" && -z ${EPREFIX} ]] ||
		die "Openmktr's initial Portage integration supports native non-Prefix builds"
	export CPPFLAGS CXXFLAGS LDFLAGS
	"${PYTHON}" packaging/gentoo/configure_build.py || die
}

src_compile() {
	eninja -f build.portage.ninja omktr
}

src_test() {
	eninja -f build.portage.ninja tests
	local check
	for check in find ninja strutil load_limit; do
		"out/${check}_test" || die "${check} tests failed"
	done
	"${PYTHON}" tests/version_generator.py || die
	OMKTR_EXPECT_NINJA_EXECUTABLE=/usr/bin/ninja-reference \
		"${PYTHON}" tests/portage.py || die
	"${PYTHON}" tests/manager.py || die
	"${PYTHON}" tests/portage_validation.py || die
	"${PYTHON}" tests/load_limits.py || die
	"${PYTHON}" tests/correctness.py || die
	"${PYTHON}" tests/workspace.py || die
	"${PYTHON}" tests/make_compat_regressions.py || die
	sh testcase/dump/run.sh || die
}

src_install() {
	dobin omktr packaging/gentoo/omktr-portage
	python_fix_shebang tools/omktr_workspace.py
	newbin tools/omktr_workspace.py omktr-workspace
	python_fix_shebang tools/omktr_manager.py
	newbin tools/omktr_manager.py omktr-manager
	if use gui; then
		insinto /usr/share/applications
		doins packaging/gentoo/openmktr-manager.desktop
	fi
	insinto /usr/share/openmktr
	doins packaging/gentoo/workspace-hooks.sh
	dodoc README.md NOTICE LICENSE LICENSES/Apache-2.0.txt
	dodoc LICENSES/upstream-AUTHORS.txt LICENSES/upstream-CONTRIBUTORS.txt
	dodoc docs/LICENSING.md
	dodoc docs/workspaces.md
	docinto portage
	dodoc packaging/gentoo/README.md
	dodoc -r packaging/gentoo/examples
}
