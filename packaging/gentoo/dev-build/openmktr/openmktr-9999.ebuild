# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0

EAPI=8
LLVM_COMPAT=( {19..22} )
PYTHON_COMPAT=( python3_{11..14} )
NINJA=ninja-reference

inherit git-r3 llvm-r2 ninja-utils python-any-r1

DESCRIPTION="Makefile executor and Ninja graph generator with opt-in Portage integration"
HOMEPAGE="https://github.com/RihardsPaps/Openmktr"
EGIT_REPO_URI="https://github.com/RihardsPaps/Openmktr.git"
EGIT_BRANCH=main

LICENSE="MPL-2.0 Apache-2.0"
SLOT="0"
KEYWORDS=""
IUSE="test"
RESTRICT="!test? ( test )"

RDEPEND="llvm-runtimes/libcxx:=
	>=dev-build/ninja-1.13"
DEPEND="llvm-runtimes/libcxx:="
BDEPEND="${PYTHON_DEPS}
	${NINJA_DEPEND}
	$(llvm_gen_dep '
		llvm-core/clang:${LLVM_SLOT}
		llvm-core/lld:${LLVM_SLOT}
	')"

pkg_setup() {
	llvm-r2_pkg_setup
	python-any-r1_pkg_setup
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
	"${PYTHON}" tests/portage_validation.py || die
	"${PYTHON}" tests/load_limits.py || die
	"${PYTHON}" tests/correctness.py || die
	"${PYTHON}" tests/make_compat_regressions.py || die
	sh testcase/dump/run.sh || die
}

src_install() {
	dobin omktr packaging/gentoo/omktr-portage
	dodoc README.md NOTICE LICENSE LICENSES/Apache-2.0.txt
	dodoc LICENSES/upstream-AUTHORS.txt LICENSES/upstream-CONTRIBUTORS.txt
	dodoc docs/LICENSING.md
	docinto portage
	dodoc packaging/gentoo/README.md
	dodoc -r packaging/gentoo/examples
}
