# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
EAPI=8
DESCRIPTION="Disposable Openmktr Portage lifecycle fixture"
HOMEPAGE="https://github.com/RihardsPaps/Openmktr"
LICENSE="MPL-2.0"
SLOT="0"
KEYWORDS="~amd64 ~arm64"
IUSE=""
BDEPEND="dev-lang/python llvm-core/clang"

src_unpack() {
	mkdir -p "${S}" || die
	cp "${FILESDIR}"/* "${S}/" || die
}

src_configure() {
	chmod +x configure || die
	econf
	if [[ ${OMKTR_FIXTURE_TEST_FALLBACK} == 1 ]]; then
		python3 -c 'from pathlib import Path; p = Path("Makefile"); p.write_text(p.read_text().replace("all check install", "all test install").replace("check: fixture", "test: fixture"))' || die
	fi
}

src_compile() {
	local before
	python3 worker.py reset || die
	emake
	python3 worker.py verify parallel || die
	before=$(python3 -c 'import os; print(os.stat("fixture").st_mtime_ns)') || die
	python3 worker.py reset || die
	emake -j1
	python3 worker.py verify serial || die
	[[ ${before} == "$(python3 -c 'import os; print(os.stat("fixture").st_mtime_ns)')" ]] ||
		die "Unchanged compiler output rebuilt"
	printf '%s\n' 'FIXTURE_VERSION = 2' > config.mk || die
	python3 worker.py reset || die
	emake -l0
	python3 worker.py verify serial || die
	[[ $(./fixture) == 'marker=7 version=2' ]] || die "Stale compile output"
	[[ $(cat build.ninja) == 'unrelated graph' ]] || die "Unrelated graph overwritten"
	if [[ ${OMKTR_PORTAGE_MODE} == ninja ]]; then
		[[ -f .openmktr/build.ninja ]] || die "Ninja compilation not selected"
	else
		[[ ! -e .openmktr ]] || die "Unexpected Ninja compilation"
	fi
	[[ ${OMKTR_FIXTURE_FAIL} != 1 ]] || emake fail
}

# Deliberately use EAPI 8's default src_test: it probes check with -n and runs
# it using MAKE rather than a custom phase that could hide adapter problems.

src_install() {
	emake DESTDIR="${D}" PACKAGE="${PN}" install
	[[ -f tested && $(cat test-phase) == test ]] || die "Default src_test did not run"
	local expected_target=check
	[[ ${OMKTR_FIXTURE_TEST_FALLBACK} != 1 ]] || expected_target=test
	[[ $(cat test-target) == "${expected_target}" ]] || die "Wrong default test target"
	[[ $(cat configure-phase) == configure ]] || die "Configuration phase missing"
	[[ $(cat install-phase) == install ]] || die "Installation phase missing"
	[[ $(cat configure-cc) == clang ]] || die "Compiler environment lost"
	if [[ -n ${OMKTR_PORTAGE_MODE} ]]; then
		[[ $(cat configure-make) == /usr/bin/omktr ]] || die "Configure did not use Openmktr"
	else
		[[ $(cat configure-make) != /usr/bin/omktr ]] || die "Control package opted in"
	fi
	insinto "/usr/share/${PN}"
	doins configure-phase compile-phase test-phase test-target install-phase configure-cc configure-make
}
