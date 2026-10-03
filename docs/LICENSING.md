# Licensing and provenance

The GNU-free Kati fork uses the [Mozilla Public License 2.0 (MPL 2.0)](../LICENSE)
for fork contributions. Original Google Kati material retains its
[Apache License 2.0](../LICENSES/Apache-2.0.txt) terms and existing rights.
Rihards Paps and Haralds Paps provide the fork terms for their respective
contributions; neither gains ownership of upstream work or the other's work.

This guide explains the repository's licensing layout. It does not add to or
replace either license. MPL 2.0 replaces PolyForm Perimeter 1.0.1 for the
current fork contributions; existing grants in earlier versions remain intact.

## Which terms apply

| Material | Terms |
| --- | --- |
| Fork modifications and new fork code, tests, build files, configuration, and documentation | MPL 2.0, except where a retained third-party notice applies. |
| Original Google Kati files and original portions of modified files | Apache License 2.0. |
| Third-party imports | Their own licenses and notices. |
| Historical license declarations and grants | Preserved; existing rights in earlier versions remain in effect. |

Mixed files retain the upstream Apache header for the original material and
a separate fork notice identifying the MPL-covered changes. The root
[NOTICE](../NOTICE) includes the MPL Exhibit A notice for fork modifications
and new fork files, including files whose format cannot carry comments.
Reproduced license texts and historical author and contributor records retain
their original provenance.

## Apache 2.0 option by negotiation

An **Apache License 2.0 option for fork contributions is available by
negotiation**. Interested parties can email
[rihardspaps6@gmail.com](mailto:rihardspaps6@gmail.com).

The Apache option requires a separate agreement covering the relevant rights.
The repository's default license for fork contributions is MPL 2.0; this
offer does not automatically license them under Apache 2.0. Original upstream
Apache grants remain available on their existing terms.

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

Retain applicable file notices in source distributions. Under MPL 2.0
section 3.1, distributed covered source, including modifications, must be
available under MPL 2.0, with information on how to obtain the license.
Under section 3.2, binary recipients must be told how to obtain the covered
source by reasonable means in a timely manner, at no more than the cost of
distribution. Shipping the license and notices alone does not satisfy that
source-availability requirement.

A standalone `ckati` executable needs accompanying license and notice files
and information on obtaining its covered source. This repository's distribution
layout does not require embedding those files in the executable.
[Dockerfile](../Dockerfile) uses `COPY . .`, and
[.dockerignore](../.dockerignore) does not exclude these files, so the test image
contains them and the source under `/workspace`.
A minimal runtime image or binary archive must include the required license,
notice, and source-access information explicitly.
External libraries and base-image components retain their own distribution requirements.

## License contributions and imports

Submit original contributions under MPL 2.0 unless a different license is
explicitly identified and accepted. Contribute only material you have the
right to license. Preserve upstream copyright, license, patent, trademark,
and attribution notices, and add or update a prominent change notice when
modifying an upstream-derived file. Keep mixed-file licensing clear.
Negotiated Apache licensing must cover the rights in the applicable contributions.

For a format that cannot carry comments, the root NOTICE supplies the MPL
Exhibit A notice. Provide an adjacent notice if additional provenance or
change information is needed, and document the exception here.
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
`LICENSE` now holds MPL 2.0.

The snapshot excludes external dependencies, downloaded campaign sources,
and user-supplied Makefiles, which retain their own terms. Update it when
performing a new licensing audit, recording both upstream and fork revisions.

## License text sources

- [Mozilla Public License 2.0 official text](https://www.mozilla.org/media/MPL/2.0/index.txt),
  reproduced verbatim in the root LICENSE.
- [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0), preserved from the
  repository's original LICENSE without editing.
