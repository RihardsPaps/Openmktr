#!/usr/bin/env python3
# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
"""Layer Gentoo flags over the portable graph without modifying it."""

import os
from pathlib import Path
import shlex


def flags(*names, required=()):
    words = [word for name in names
             for word in shlex.split(os.environ.get(name, ''))]
    return shlex.join([*words, *required]).replace('$', '$$')


def main():
    compile_flags = flags('CPPFLAGS', 'CXXFLAGS', required=(
        '-std=c++17', '-DNOLOG', '-Wall', '-Wextra', '-stdlib=libc++',
        '-pthread', '-ffunction-sections', '-fdata-sections',
        '-DOMKTR_NINJA_EXECUTABLE="/usr/bin/ninja-reference"'))
    link_flags = flags('CXXFLAGS', 'LDFLAGS', required=(
        '-stdlib=libc++', '-fuse-ld=lld', '-pthread'))
    Path('build.portage.ninja').write_text(
        f'include build.ninja\ncxx = clang++\n'
        f'cxxflags = {compile_flags}\nldflags = {link_flags}\n', encoding='utf-8')


if __name__ == '__main__':
    main()
