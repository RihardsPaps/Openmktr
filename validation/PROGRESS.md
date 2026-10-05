# Compatibility campaign results

All 41 in-scope entries marked as passing below have successful builds.
NetBSD 10.1 is closed as out of scope because its build system relies on BSD
Make assumptions that Openmktr does not target. No complete NetBSD build is
claimed.

The matrix records specific releases and configurations; a passing result does
not establish compatibility with every version or target. Read the
[campaign guide](README.md) before using the tools or citing a row.

## Evidence boundary

A project pass requires a real build using this repository's `ckati` for
Make-driven stages, plus the recorded source revision, configuration, command,
exit status, artifact checks, repeat build, and source-integrity checks.
Success through another build system alone does not establish Kati compatibility.
A phase marked PASS establishes only that phase's exit status.

The campaign began on 2026-09-26 at repository baseline `d25e7be`. Its later
builder was Ubuntu 24.04 in WSL2, x86_64, with 16 assigned processors,
24 GiB RAM, and 16 GiB swap. A pinned Chimera chroot supplied the canonical
musl/LLVM checks. Sources and builds used native Linux storage under
`/root/universal-tool-campaign-20260927`. The campaign tool used O2/ThinLTO;
job limits were bounded by memory, with compiler caches where supported.

The latest recorded promoted WSL binary has SHA-256
`39a2c8c162d330aa7246ce8c074ad486773a99f089f31fd31db053388e7d69c9`.
Its checkpoint reports 105 focused cases, 855/855 canonical Chimera scenarios,
and zero quarantined crashes. These checks were not rerun for this documentation edit.

## Coverage limits

- Linux 6.12 built for all 21 architecture directories. x86 has QEMU boot evidence; the
  other cross-built architectures have artifact checks without boot runs.
- coreboot 26.06 required its upstream-generated `xcompile` file to be staged before the
  selected build.
- The firmware stack passed with TF-A 2.10.0. The TF-A 2.14.0 macro-parser issue
  is resolved for `PLAT=qemu SPD=opteed`: Openmktr generated a Ninja graph that
  built BL1, BL2, and BL31. FIP packaging and QEMU boot were not verified for 2.14.0.
- Yocto's selected image used `PTEST_PARALLEL_MAKE=-j1` for elfutils and QEMU's
  IvyBridge CPU. Its successful run reported 12 BitBake warnings, including
  diagnostic-task taint notices.
- Guix's generated manpage was restored from the release archive before the final
  source-integrity check. No source patch is counted toward a pass.
- NetBSD 10.1 is out of scope because its top-level `Makefile` uses BSD
  `.if`/`.endif` directives. Openmktr does not target the BSD Make dialect, and
  no complete NetBSD cross-build is claimed.

## Project matrix (requested order)

| # | Project | Status | Evidence / configuration |
|---|---|---|---|
| 1 | GNU binutils | PASS (selected configuration) | 2.44 x86_64 musl host; direct and generated Ninja clean builds, artifact smoke and repeat no-op builds |
| 2 | GCC + binutils + glibc toolchain bootstrap | PASS (selected configuration) | x86_64 binutils 2.44 + GCC 14.2 C/C++ + glibc 2.40 + Linux 6.12 headers; installed compiler, C smoke and source comparisons PASS |
| 3 | LibreOffice | PASS (selected configuration) | 25.2.7.2 archive SHA256 `21b38b7f429d305b229fb70f3294dd6ccd803f52dad0463d0923cdf143e9c8bd`; full ckati -j2 build, installed soffice PDF conversion, repeat build and original source comparison PASS |
| 4 | QEMU | PASS (selected configuration) | 9.2.2 all 61 default system/user targets; artifacts, image round trip, repeat build and source integrity PASS |
| 5 | PostgreSQL | PASS (selected configuration) | 17.4 world-bin/contrib, install, unprivileged SQL/extensions smoke, repeat build and original source integrity PASS |
| 6 | MySQL | PASS (selected configuration) | 8.4.4 CMake Unix Makefiles through ckati; complete build/install, private-socket server SQL smoke, repeat and source integrity PASS |
| 7 | Wine | PASS (selected configuration) | 10.0 win64 Clang PE clean build; PE artifacts, wine64 version, cmd runtime smoke, repeat build and source integrity PASS |
| 8 | FFmpeg | PASS (selected configuration) | 7.1.1 build; H.264 encode/decode 12-frame smoke, repeat build and source integrity PASS |
| 9 | OpenSSL | PASS (selected configuration) | 3.4.1 build, 3,819 upstream tests, version, repeat build and original source integrity PASS |
| 10 | LLVM/Clang | PASS (selected configuration) | 19.1.7 Clang/lld/clang-tools-extra and all LLVM targets; ckati build/install, installed compiler/linker and bitcode runtime checks, repeat and source integrity PASS |
| 11 | GCC with all supported languages | PASS (selected configuration) | GCC 14.2 c,ada,c++,d,fortran,go,jit,lto,m2,objc,obj-c++,rust ckati build/install; 12 frontend/runtime smoke categories, repeat and 134,851-entry source integrity PASS |
| 12 | GDB | PASS (selected configuration) | 17.2 all debugger targets and simulators; build/install, breakpoint/variable smoke, repeat and source integrity PASS |
| 13 | GNU Autotools | PASS (selected component set) | M4 1.4.21, Autoconf 2.73, Automake 1.18.1, Libtool 2.5.4 through ckati; upstream suites including Automake 2,960 tests with zero unexpected failures, generated-project build/test/install, repeat and source integrity PASS |
| 14 | GNU Emacs | PASS (selected configuration) | 30.2 text-only, without native compilation or tree-sitter; ckati build/install, batch Lisp smoke, repeat build, source integrity PASS |
| 15 | GNU Guile | PASS (selected configuration) | 3.0.11 ckati build/install, Scheme runtime smoke, repeat build with release Info preservation, source integrity PASS |
| 16 | U-Boot | PASS (selected configuration) | 2025.01 qemu_arm64 cold defconfig + one-step ckati build, QEMU boot banner, repeat, source integrity PASS |
| 17 | BusyBox | PASS (selected configuration) | 1.36.1 out-of-tree defconfig with CONFIG_TC disabled for host UAPI; 401 applets, shell/tr smoke, repeat build, source integrity PASS |
| 18 | Buildroot | PASS (selected configuration) | 2025.02.18 `qemu_x86_64_defconfig`; ckati kernel/ext2 image build, bundled QEMU serial login, repeat and original-source integrity PASS |
| 19 | OpenWrt | PASS (selected configuration) | 25.12.5 x86_64 full `world` through ckati, 14 generated checksums verified, QEMU serial root shell, repeat `world` and pristine-source integrity PASS |
| 20 | Yocto/OpenEmbedded | PASS (selected configuration) | Poky 5.0.10 qemux86-64 `core-image-minimal` through BitBake with `MAKE=ckati` for Make-driven tasks; 4076/4076 tasks succeeded, ext4/kernel/manifest verified, IvyBridge QEMU root-shell probe, all-task repeat and clean source PASS |
| 21 | coreboot | PASS (selected configuration) | 26.06 QEMU x86_64 i440fx ROM through ckati, QEMU bootblock/romstage log, repeat and source integrity PASS; upstream-generated `xcompile` file pre-staged after tool rejected its first generated-file check |
| 22 | GRUB | PASS (selected configuration) | 2.14 x86_64 EFI out-of-tree ckati build/install, generated PE32+ EFI image/script smoke, repeat and source integrity PASS |
| 23 | TF-A + OP-TEE + U-Boot firmware stack | PASS (selected configuration) | TF-A 2.10.0 + OP-TEE 4.9.0 + U-Boot 2025.01 through ckati; QEMU secure/normal UART boot, repeat and source integrity PASS. TF-A 2.14.0 parser issue resolved: Openmktr-generated Ninja built the qemu/opteed `all` target (BL1/BL2/BL31); FIP packaging and boot not verified |
| 24 | MariaDB | PASS (selected configuration) | 11.4.5 out-of-tree CMake Unix Makefiles via ckati; build/install, private-socket server SQL smoke, repeat and source integrity PASS |
| 25 | SQLite | PASS (selected configuration) | 3.53.4 official autoconf archive (SHA3-256 `454e45f61c6bd75b7420e7190732dea03ce6639c63ada47bbc592f67fc340338`); ckati build/install, persisted SQL and integrity smoke, repeat build, source integrity PASS; release Makefile has no `check` target |
| 26 | Redis | PASS (selected configuration) | 8.4.7 official release archive SHA256 `ed83df9fcb724176fc901147aaa63830166e35c1cff39835c4fc9a5860ed1310`; separate build tree, ckati -j2 TLS build, all 144 upstream suites passed, private-socket commands, repeat and original-source integrity PASS |
| 27 | Git | PASS (selected configuration) | 2.49.0 archive SHA256 `618190cf590b7e9f6c11f91f23b1d267cd98c3ab33b850416d8758f8b5a85628`; separate build tree, ckati build/install, upstream suite: 29,836 successes, 0 failures, 270 marked broken, commit/clone/history smoke, repeat and original-source integrity PASS |
| 28 | PostgreSQL extension ecosystem | PASS (selected set) | PostgreSQL 17.4 contrib plus pgvector 0.8.6 and PostGIS 3.6.4; both external extensions ckati build/install, HNSW or GIS SQL smoke, repeat and source integrity PASS |
| 29 | Mesa | PASS (selected configuration) | 19.0.8 pinned Autotools OSMesa-only release; ckati build/install, installed off-screen GL pixel test, repeat and 6,235-entry original-source integrity PASS; 19.2.8 is Meson-only |
| 30 | X.org | PASS (selected component set) | xorgproto 2024.1, libX11 1.8.12, libXext 1.3.6 ckati build/install; installed Xlib/Xext client, repeat and source integrity PASS |
| 31 | ImageMagick | PASS (selected configuration) | 7.1.2-32 module-enabled out-of-tree ckati build/install, PNG/JPEG conversion smoke, repeat and source integrity PASS |
| 32 | SDL | PASS (selected configuration) | SDL3 3.4.16 out-of-tree ckati build/install, 25/25 CTest cases, installed dummy-video smoke, repeat and source integrity PASS |
| 33 | VLC | PASS (selected configuration) | 3.0.24 headless ckati build/install, installed unprivileged WAV playback, repeat and original-source integrity PASS |
| 34 | OpenBLAS | PASS (selected configuration) | 0.3.29 `DYNAMIC_ARCH=1`, 16 threads; ckati build/bundled tests/install, CBLAS `dgemm` and LAPACKE `dgesv` smoke, repeat and source integrity PASS |
| 35 | LAPACK | PASS (selected configuration) | 3.12.1 reference BLAS/LAPACK/TMGLIB through ckati; upstream numerical suite 5,200,645 tests/zero failures, `dgesv`/`dgemm` smoke, repeat and source integrity PASS |
| 36 | PETSc | PASS (selected configuration) | 3.25.5 OpenMPI 5.0.6 + OpenBLAS 0.3.29; ckati build/install, 1/2-rank upstream examples, repeat and original-source integrity PASS |
| 37 | OpenMPI | PASS (selected configuration) | 5.0.6 out-of-tree GNU14 configure/build/install through ckati; installed two-rank Allreduce smoke, repeat and source integrity PASS |
| 38 | MPICH | PASS (selected configuration) | 4.2.3 out-of-tree GNU14 configure/build/install through ckati; installed two-rank Allreduce smoke, repeat and source integrity PASS |
| 39 | GNU Make | PASS (selected configuration) | 4.4.1 ckati build/install, 1,444 upstream tests in 134 categories as unprivileged user, smoke, repeat, source integrity PASS |
| 40 | Linux kernel × all architectures | BUILD PASS 21/21; runtime coverage limited | Pinned 6.12; every `arch/` directory passed ckati `defconfig` and full image build/repeat out-of-tree; expected binary format verified for each, x86 `bzImage` QEMU boot, 86,680 original source files/symlinks unchanged. Cross-built architectures have artifact checks but no boot runs |
| 41 | NetBSD 10.1 | CLOSED — OUT OF SCOPE | Its build files rely on BSD Make `.if`/`.endif` conditionals, which Openmktr does not target. No complete NetBSD build is claimed. |
| 42 | GNU Guix | PASS (selected configuration) | 1.5.0 ckati build/install, installed CLI/hash smoke, repeat build and restored pristine 3,513-entry source archive PASS |

## Historical evidence and new phases

[The archived record](history.log) preserves the previous campaign narrative,
interim failures, fixes, command ledger, and issue #20 investigation. Read those
entries chronologically: earlier TESTING or failed states do not override the
final matrix above. Full command output remains in the ignored `validation/logs/`
directory where available; the archive references those filenames.

`phase.py` continues to append new command and result entries below. A new
phase does not update the matrix automatically. Update a row only after its
configuration and acceptance evidence have been verified.

## New phase log

### TF-A 2.14.0 macro-parser follow-up (2026-10-06)

With Openmktr commit `7399826` and TF-A v2.14.0 (`1d5aa939bc8d3d892e2ed9945fa50e36a1a924cc`),
generated a Ninja graph for `PLAT=qemu SPD=opteed` and built the `all` target
with `sh ./ninja.sh -j4`. All 245 Ninja steps completed, producing
`build/qemu/release/bl1.bin`, `bl2.bin`, and `bl31.bin`. This resolves the
reported macro-parser failure for this configuration. FIP packaging and a QEMU
boot were not verified because the required OP-TEE transfer-list payloads and
`qemu-system-aarch64` were unavailable.

Openmktr printed a nonfatal parse warning for TF-A's tab-indented `$(error ...)`
inside an inactive `tsram` conditional. GNU Make's dry run skips the inactive
branch, and the Openmktr build succeeds; the warning does not indicate that the
error function ran.
