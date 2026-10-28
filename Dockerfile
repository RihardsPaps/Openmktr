# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted build, CI, or repository configuration for the GNU-free fork.
# Run portable Portage adapter and load-limit checks in the reference gate.
# Check cross-version campaign evidence accounting without installing ccache.
# Check paired Ninja benchmark evidence accounting without release sources.
# Check persistent workspaces, idle compression and their distribution helper.
# Exercise native graph optimization behavior in the portable gate.
# Lock the refreshed base packages and require modern Ninja jobserver checks.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original notices below remain applicable.

FROM docker.io/chimeralinux/chimera@sha256:29102d7e12a1f464707d7aba19ce53e652d277861838ed4129178d0655444b1a

# Chimera ships Clang/LLVM, musl, libc++, and a BSD-derived userland.
COPY tools/chimera-packages.lock tools/install_reference_packages.sh tools/check_reference_packages.py /toolchain/tools/
WORKDIR /toolchain
RUN sh tools/install_reference_packages.sh
RUN for package in gcc glibc bash coreutils findutils; do \
      if apk info -e "$package" >/dev/null 2>&1; then \
        echo "unexpected GNU package: $package" >&2; exit 1; \
      fi; \
    done

WORKDIR /workspace
COPY . .
ARG OMKTR_SOURCE_REVISION
ENV OMKTR_SOURCE_REVISION=$OMKTR_SOURCE_REVISION
RUN ninja -f build.ninja -j 4 omktr tests
RUN if ldd omktr | grep -E 'libstdc\+\+|libgcc|libc\.so\.6'; then exit 1; fi

CMD ["/bin/sh", "-c", "out/find_test && out/ninja_test && out/strutil_test && out/load_limit_test && python tests/version_generator.py && python tests/correctness.py && python tests/graph_optimizations.py && OMKTR_TEST_GRAPH_PROFILE=aggressive python tests/correctness.py && python tests/make_compat_regressions.py && python tests/portage.py && python tests/load_limits.py && python tests/portage_validation.py && python tests/cross_version_cache.py && python tests/ninja_benchmark.py && python tests/workspace.py && python tests/ninja_jobserver.py && python tests/release_packaging.py && python tests/regression.py && OMKTR_TEST_GRAPH_PROFILE=aggressive python tests/regression.py && sh testcase/dump/run.sh"]
