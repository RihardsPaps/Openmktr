FROM docker.io/chimeralinux/chimera@sha256:29102d7e12a1f464707d7aba19ce53e652d277861838ed4129178d0655444b1a

# Chimera ships Clang/LLVM, musl, libc++, and a BSD-derived userland.
RUN apk add clang ninja python chimerautils
RUN for package in gcc glibc bash coreutils findutils; do \
      if apk info -e "$package" >/dev/null 2>&1; then \
        echo "unexpected GNU package: $package" >&2; exit 1; \
      fi; \
    done

WORKDIR /workspace
COPY . .
ARG KATI_SOURCE_REVISION
ENV KATI_SOURCE_REVISION=$KATI_SOURCE_REVISION
RUN ninja -f build.ninja -j 4 ckati tests
RUN if ldd ckati | grep -E 'libstdc\+\+|libgcc|libc\.so\.6'; then exit 1; fi

CMD ["/bin/sh", "-c", "out/find_test && out/ninja_test && out/strutil_test && python tests/correctness.py && python tests/regression.py && sh testcase/dump/run.sh"]
