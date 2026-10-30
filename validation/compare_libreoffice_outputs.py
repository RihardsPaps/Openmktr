#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Compare complete installed payloads and compiler objects in two local lanes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import zipfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def files(root):
    return {str(p.relative_to(root)): p for p in root.rglob('*')
            if p.is_file() or p.is_symlink()}


def zip_payload(path):
    with zipfile.ZipFile(path) as archive:
        # Keep duplicate names and every member's full decompressed contents.
        occurrences = {}
        payloads = []
        for member in archive.infolist():
            index = occurrences.get(member.filename, 0)
            occurrences[member.filename] = index + 1
            payloads.append((member.filename, index, member.is_dir(),
                             hashlib.sha256(archive.read(member)).hexdigest()))
        return sorted(payloads)


def compare(gnu, candidate):
    for lane in (gnu, candidate):
        if (not (lane / 'build/instdir/program/soffice.bin').is_file() or
                not (lane / 'build/workdir').is_dir()):
            raise ValueError('Incomplete LibreOffice lane: ' + str(lane))
    left, right = files(gnu / 'build/instdir'), files(candidate / 'build/instdir')
    result = dict(missing=sorted(left.keys() - right.keys()),
                  extra=sorted(right.keys() - left.keys()), raw_identical=[],
                  zip_payload_identical=[], prefix_identical=[], different=[],
                  mode_differences=[])
    result['unsupported_entries'] = {
        label: [str(p.relative_to(lane / 'build/instdir'))
                for p in (lane / 'build/instdir').rglob('*')
                if not (p.is_file() or p.is_dir() or p.is_symlink())]
        for label, lane in (('gnu', gnu), ('candidate', candidate))}
    for name in sorted(left.keys() & right.keys()):
        a, b = left[name], right[name]
        if stat.S_IMODE(a.lstat().st_mode) != stat.S_IMODE(b.lstat().st_mode):
            result['mode_differences'].append(name)
        if a.is_symlink() or b.is_symlink():
            same = a.is_symlink() and b.is_symlink() and os.readlink(a) == os.readlink(b)
        else:
            same = digest(a) == digest(b)
        if same:
            result['raw_identical'].append(name)
        elif (not a.is_symlink() and not b.is_symlink() and
              zipfile.is_zipfile(a) and zipfile.is_zipfile(b) and
              zip_payload(a) == zip_payload(b)):
            result['zip_payload_identical'].append(name)
        elif (name.endswith('-gdb.py') and
              a.read_bytes().replace(os.fsencode(gnu), b'<LANE>') ==
              b.read_bytes().replace(os.fsencode(candidate), b'<LANE>')):
            result['prefix_identical'].append(name)
        else:
            result['different'].append(name)
    a = {str(p.relative_to(gnu / 'build/workdir')): p
         for p in (gnu / 'build/workdir').rglob('*.o')}
    b = {str(p.relative_to(candidate / 'build/workdir')): p
         for p in (candidate / 'build/workdir').rglob('*.o')}
    if not a or not b:
        raise ValueError('Compiler object comparison cannot be empty')
    result['objects'] = dict(missing=sorted(a.keys() - b.keys()),
                             extra=sorted(b.keys() - a.keys()),
                             different=[n for n in sorted(a.keys() & b.keys())
                                        if digest(a[n]) != digest(b[n])],
                             gnu_count=len(a), candidate_count=len(b))
    directories = []
    for lane in (gnu, candidate):
        root = lane / 'build/instdir'
        directories.append({str(p.relative_to(root)): stat.S_IMODE(p.stat().st_mode)
                            for p in [root, *root.rglob('*')]
                            if p.is_dir() and not p.is_symlink()})
    a, b = directories
    result['directories'] = dict(
        missing=sorted(a.keys() - b.keys()), extra=sorted(b.keys() - a.keys()),
        mode_differences=[n for n in sorted(a.keys() & b.keys()) if a[n] != b[n]],
        gnu_count=len(a), candidate_count=len(b))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gnu', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refusing to overwrite comparison evidence')
    result = compare(args.gnu.resolve(), args.candidate.resolve())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    pending = any(result[k] for k in ('missing', 'extra', 'different',
                                     'mode_differences')) or any(
        result['objects'][k] for k in ('missing', 'extra', 'different')) or any(
        result['directories'][k] for k in ('missing', 'extra', 'mode_differences'))
    pending |= any(result['unsupported_entries'].values())
    print('Differences require verification' if pending else 'Outputs match')
    return int(pending)


if __name__ == '__main__':
    raise SystemExit(main())
