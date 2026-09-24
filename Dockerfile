FROM chimeralinux/chimera:latest

# Chimera ships Clang/LLVM, musl, libc++, and a BSD-derived userland.
RUN apk add clang ninja python chimerautils
RUN for package in gcc glibc bash coreutils findutils; do \
      if apk info -e "$package" >/dev/null 2>&1; then \
        echo "unexpected GNU package: $package" >&2; exit 1; \
      fi; \
    done

WORKDIR /workspace
COPY . .
RUN ninja -f build.ninja -j 4 ckati tests
RUN if ldd ckati | grep -E 'libstdc\+\+|libgcc|libc\.so\.6'; then exit 1; fi

CMD ["/bin/sh", "-c", "out/find_test && out/ninja_test && out/strutil_test && python tests/regression.py && sh testcase/dump/run.sh"]
