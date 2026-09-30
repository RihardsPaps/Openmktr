# Licensing and provenance

The combined GNU-free Kati fork is distributed under
[PolyForm Perimeter 1.0.1](../LICENSE). Original Google Kati material retains
its [Apache License 2.0](../LICENSES/Apache-2.0.txt) terms and existing rights.
Rihards Paps and Haralds Paps provide the fork terms for their respective
contributions; neither gains ownership of upstream work or the other's work.

This guide explains the repository's licensing layout. It does not add to or
replace either license. The combined fork is source-available. PolyForm
Perimeter restricts providing competing products to others, including free
products, services, libraries, and plug-ins as defined in that license.

## Which terms apply

| Material | Terms |
| --- | --- |
| Combined fork distributed from the licensing change onward | PolyForm Perimeter 1.0.1, with retained upstream licenses and notices. |
| Original Google Kati files and original portions of modified files | Apache License 2.0. |
| Fork modifications and new fork code, tests, build files, configuration, and documentation | PolyForm Perimeter 1.0.1, except where a retained third-party notice applies. |
| Third-party imports | Their own licenses and notices. |
| Historical Apache declarations and grants | Preserved; existing rights in the underlying Apache material remain in effect. |

Mixed files carry both licenses through their notices. The upstream Apache
header covers the original material; the separate fork notice identifies the
changes and points here for their terms. This is not an optional dual-license
offer for the combined fork. Do not describe that fork as Apache-only or as
unrestricted open-source software.

At the licensing change, the maintainers reported that the repository had
remained private and had not been shared externally. That change prepared the
first external distribution; it did not imply publication of earlier fork revisions.

## Preserve attribution and change notices

[NOTICE](../NOTICE) identifies the source project and fork. The original
[author record](../LICENSES/upstream-AUTHORS.txt) and
[contributor record](../LICENSES/upstream-CONTRIBUTORS.txt) are retained verbatim.
They distinguish copyright authors from contributors; contributor credit alone
does not establish copyright ownership. Their historical CLA instructions are
not this fork's contribution policy.

Modified upstream files carry a prominent `FORK MODIFICATION NOTICE (2026)`.
Keep original copyright and license headers intact. Fixture notices appear at
the end of Makefiles to preserve parsing behavior and diagnostic line numbers.
A change notice identifies the fork's modifications without claiming authorship
of the original file or joint ownership of every change.

## Redistribute source or binaries

Include these files with source and binary distributions:

- [LICENSE](../LICENSE)
- [LICENSES/Apache-2.0.txt](../LICENSES/Apache-2.0.txt)
- [NOTICE](../NOTICE)
- [LICENSES/upstream-AUTHORS.txt](../LICENSES/upstream-AUTHORS.txt)
- [LICENSES/upstream-CONTRIBUTORS.txt](../LICENSES/upstream-CONTRIBUTORS.txt)
- This licensing guide, so the scope of each license remains clear.

Retain applicable file notices in source distributions. A standalone `ckati`
executable needs accompanying license and notice files; this repository's
distribution layout does not require embedding them in the executable.

[Dockerfile](../Dockerfile) uses `COPY . .`, and
[.dockerignore](../.dockerignore) does not exclude these files, so the test image
contains them under `/workspace`.
A minimal runtime image or binary archive must copy them explicitly.
External libraries and base-image components retain their own distribution requirements.

## License contributions and imports

Submit original contributions under PolyForm Perimeter 1.0.1 unless a different
license is explicitly identified and accepted. Contribute only material you
have the right to license. Preserve upstream copyright, license, patent,
trademark, and attribution notices, and add or update a prominent change notice
when modifying an upstream-derived file. Keep mixed-file licensing clear.

For a format that cannot carry comments, provide an adjacent notice identifying
the exact path, provenance, and changes, and document the exception here.
Do not alter parser-sensitive fixture bytes merely to insert a header.
Review third-party imports and include their applicable licenses and notices.

## Read the provenance audit

[licensing-inventory.tsv](licensing-inventory.tsv) is an audit snapshot of
566 tracked files at fork revision `b77175358599c6550015bc121d38c1c9ff6a6aa5`,
compared with upstream `12685f9a31b7bd30e20a0cd9b939ea3eb0640d45`, the parent
of the initial fork snapshot. It identifies 91 modified upstream files and
distinguishes unchanged upstream files from fork additions. It is not a
current file inventory or an alternative license grant.

The audit found no separate non-Apache license in retained upstream files,
no removed copyright or license header in surviving modified sources or
fixtures, and no additional upstream copies among fork additions using Git
copy/rename detection. The upstream baseline had no `NOTICE` file. Its root
Apache `LICENSE` is preserved in `LICENSES/Apache-2.0.txt`; the fork's root
`LICENSE` holds PolyForm. Reproduced license texts and historical records
retain their original provenance; the inventory and licensing guide are fork
documentation.

The snapshot excludes external dependencies, downloaded campaign sources,
and user-supplied Makefiles, which retain their own terms. Update it when
performing a new licensing audit, recording both upstream and fork revisions.

## License text sources

- [PolyForm Perimeter 1.0.1 source
  text](https://github.com/polyformproject/polyform-licenses/blob/1.0.0/PolyForm-Perimeter-1.0.1.md),
  Git blob `088231fb5ec6ed13bdcd3e67f4c2b0f404eb0568`.
- [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0), preserved from the
  repository's original `LICENSE` without editing.
