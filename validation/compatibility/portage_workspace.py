#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Qualify retained workspaces in an explicitly disposable Gentoo container.

Requires an installed candidate, helper and hooks. Installs an original fixture
through three EAPI 8 releases; never run against a user's Portage configuration.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


PACKAGE = "omktr-workspace-fixture"
CACHE = Path("/var/cache/omktr-workspace-validation")


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (os.environ.get("OMKTR_PORTAGE_VALIDATION") != "1" or os.geteuid() != 0 or
            not Path("/etc/gentoo-release").exists()):
        parser.error("requires a disposable Gentoo container and OMKTR_PORTAGE_VALIDATION=1")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()) or CACHE.exists():
        parser.error("output and validation cache must be new")
    result = dict(status="running", started=time.time(), samples=[], commands=[],
                  image_id=os.environ.get("OMKTR_VALIDATION_IMAGE_ID"),
                  binary_sha256=checksum(Path("/usr/bin/omktr")),
                  helper_sha256=checksum(Path("/usr/bin/omktr-workspace")))

    def run(command, name):
        started = time.monotonic()
        with (output / (name + ".log")).open("x") as stream:
            code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                  timeout=300).returncode
        result["commands"].append(dict(command=command, status=code,
                                       seconds=time.monotonic() - started, log=name + ".log"))
        if code:
            raise RuntimeError(f"{name} failed with {code}; see {output}")

    try:
        overlay = output / "overlay"
        (overlay / "profiles").mkdir(parents=True)
        (overlay / "profiles/repo_name").write_text("omktr-workspace-validation\n")
        (overlay / "metadata").mkdir()
        (overlay / "metadata/layout.conf").write_text("masters = gentoo\nthin-manifests = true\n")
        package = overlay / "dev-build" / PACKAGE
        package.mkdir(parents=True)
        ebuild = r'''# SPDX-License-Identifier: MPL-2.0
EAPI=8
DESCRIPTION="Original retained incremental workspace fixture"
SLOT="0"
LICENSE="MPL-2.0"
KEYWORDS="~amd64 ~arm64"
BDEPEND="llvm-core/clang dev-lang/python"
src_unpack() {
    mkdir -p "${S}" || die
    cd "${S}" || die
    local value=1
    [[ ${PV} != 3 ]] || value=3
    printf '#include <stdio.h>\nint b(void); int main(void) { printf("%%d\\n", %s + b()); }\n' "${value}" > a.c || die
    printf 'int b(void) { return 2; }\n' > b.c || die
    printf 'all: app\napp: a.o b.o\n\t@$(CC) a.o b.o -o app\na.o: a.c\n\t@$(CC) -c a.c -o a.o\nb.o: b.c\n\t@$(CC) -c b.c -o b.o\ncheck: app\n\t@./app\ninstall: app\n\t@mkdir -p $(DESTDIR)/usr/bin; cp app $(DESTDIR)/usr/bin/omktr-workspace-fixture\n' > Makefile.in || die
}
src_configure() {
    cmp -s Makefile.in Makefile || cp Makefile.in Makefile || die
}
src_compile() {
    emake
    local maintenance
    maintenance=$(/usr/bin/omktr workspace gc --root "${OMKTR_WORKSPACE_ROOT}" --idle-seconds 0) || die
    [[ ${maintenance} == *'"packed_workspaces": 0'* ]] || die "GC packed an active Portage owner"
}
src_install() { emake DESTDIR="${D}" install; }
'''
        for version in (1, 2, 3):
            (package / f"{PACKAGE}-{version}.ebuild").write_text(ebuild)
        config = Path("/etc/portage")
        settings = {
            "repos.conf/omktr-workspace-validation.conf":
                f"[omktr-workspace-validation]\nlocation = {overlay}\nmasters = gentoo\nauto-sync = no\n",
            "package.accept_keywords/omktr-workspace-validation": f"dev-build/{PACKAGE} **\n",
            "package.env/omktr-workspace-validation": f"dev-build/{PACKAGE} omktr-workspace-validation\n",
            "env/omktr-workspace-validation":
                'export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="ninja"\n'
                f'export OMKTR_WORKSPACE_ROOT="{CACHE}"\n'
                'CC="clang"\nCXX="clang++"\nCFLAGS="-O1"\nMAKEOPTS="-j4"\n'
                'FEATURES="${FEATURES} test -keepwork -keeptemp ipc-sandbox network-sandbox pid-sandbox"\n',
            "bashrc": 'if [[ -n ${OMKTR_WORKSPACE_ROOT} ]]; then\n'
                      '    source /usr/share/openmktr/workspace-hooks.sh\nfi\n',
        }
        for relative, value in settings.items():
            path = config / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("x") as stream:
                stream.write(value)
        CACHE.mkdir()
        shutil.chown(CACHE, user="portage", group="portage")
        run(["emerge", "--info"], "environment")
        for version in (1, 2, 3):
            run(["emerge", "--oneshot", "--verbose", "--usepkg=n", "--autounmask=n",
                 f"=dev-build/{PACKAGE}-{version}::omktr-workspace-validation"], f"version-{version}")
            states = list(CACHE.glob("*/*/state.json"))
            assert len(states) == 1, states
            state = json.loads(states[0].read_text())
            directory = states[0].parent / "content/source"
            sample = dict(version=version, directory=str(directory), files={
                name: dict(mtime_ns=(directory / name).stat().st_mtime_ns,
                           sha256=checksum(directory / name))
                for name in ("a.o", "b.o", "app")})
            sample["output"] = subprocess.check_output(["/usr/bin/" + PACKAGE], text=True).strip()
            assert sample["output"] == ("5" if version == 3 else "3"), sample
            result["samples"].append(sample)
            if version == 2:
                assert result["samples"][0]["directory"] == str(directory)
                assert result["samples"][0]["files"] == sample["files"], "identical release rebuilt"
                run(["/usr/bin/omktr", "workspace", "gc", "--root", str(CACHE),
                     "--idle-seconds", "0"], "pack-between-versions")
                assert json.loads(states[0].read_text())["packed"]
            if version == 3:
                assert result["samples"][1]["files"]["b.o"] == sample["files"]["b.o"]
                assert result["samples"][1]["files"]["a.o"] != sample["files"]["a.o"]
            assert not Path(state["source"]).exists(), "Portage did not clean its versioned sources"
        result["status"] = "passed"
    except BaseException as error:
        result.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        result["finished"] = time.time()
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
