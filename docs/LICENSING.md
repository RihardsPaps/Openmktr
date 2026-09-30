# Licensing and provenance

The GNU-free Kati fork is distributed as a derivative work under the
[PolyForm Perimeter License 1.0.1](../LICENSE). Rihards Paps and Haralds Paps
provide these terms for their respective fork contributions. This does not
assign them ownership of upstream work or of one another's contributions.

The maintainers report that this repository has remained private and has not
been shared externally. This licensing change prepares the first external
distribution; it does not imply that earlier fork revisions were published.

## Which terms apply

| Material | Applicable terms |
| --- | --- |
| Combined GNU-free Kati fork distributed from this licensing change onward | PolyForm Perimeter 1.0.1, with the retained upstream licences and notices below. |
| Original Google Kati material, including unchanged files and original portions of modified files | Apache License 2.0; see the preserved text in `LICENSES/Apache-2.0.txt`. |
| Fork modifications and original new fork code, tests, build/configuration files, and documentation | PolyForm Perimeter 1.0.1, except where a retained third-party notice applies. |
| Third-party material, including material subsequently imported | Its own applicable licence and notices; the repository's default does not override them. |
| Historical Apache declarations and existing Apache rights | Preserved; this change does not revoke existing grants or restrict rights in the underlying Apache material. |

Both licences accompany mixed files. An upstream Apache header describes the
upstream material; the separate fork modification notice identifies the fork's
changes and points here for their terms. The two licences are not an optional
dual-licence offer for the combined fork. Recipients retain their Apache rights
to the underlying Apache material independently of this distribution.

PolyForm Perimeter permits use for purposes other than providing a competing
product to others, as defined by the licence. The restriction can include free
products, services, libraries, and plug-ins. This fork is **source-available**;
do not describe the combined fork as Apache-only or as unrestricted open-source
software. These explanations do not add to or replace either licence's terms.

## Attribution and modification notices

[NOTICE](../NOTICE) identifies the source project and the fork. The original
[upstream author record](../LICENSES/upstream-AUTHORS.txt) and
[upstream contributor record](../LICENSES/upstream-CONTRIBUTORS.txt) are retained
verbatim as historical records. Authors and contributors are distinct: those
records do not say that every contributor owns the copyright in their work.
Their historical CLA instructions are not the contribution policy of this fork.

Modified upstream files carry an explicit `FORK MODIFICATION NOTICE (2026)`.
Makefile fixture notices appear at the end of the file to preserve diagnostic
line numbers and parsing tests. Existing copyright and licence headers remain
intact. A modification notice identifies the fork's changes; it is not a claim
of authorship of the original file or of joint ownership of every change.

The [provenance inventory](licensing-inventory.tsv) covers all 566 tracked files
at fork revision `b77175358599c6550015bc121d38c1c9ff6a6aa5`, compared with upstream
`12685f9a31b7bd30e20a0cd9b939ea3eb0640d45`, the parent of the initial fork snapshot.
It distinguishes 91 modified upstream files from unchanged upstream and added
fork files. It is an audit snapshot, not an alternative licence grant. The root
Apache `LICENSE` from that snapshot is preserved in `LICENSES/Apache-2.0.txt`;
the root `LICENSE` now holds PolyForm. New licensing documentation and the
inventory are fork documentation; reproduced licence texts and historical
records retain their original provenance.

The baseline includes no `NOTICE` file. Its separate author and contributor
records are preserved here rather than discarded during documentation
consolidation. The audit found no separate non-Apache licence in the retained
upstream files and no removed copyright/licence header in surviving modified
source or fixture files. Git copy/rename detection found no additional copied
upstream files among the fork additions. This inventory does not cover external
dependencies, downloaded compatibility-campaign source trees, or user-supplied
Makefiles; those retain their own terms.

## Redistribution

Include `LICENSE`, `LICENSES/Apache-2.0.txt`, `NOTICE`, and the preserved upstream
attribution records with source and binary distributions, and retain relevant
file notices in source distributions. Carry the licensing guide with them so
that the scope of each licence remains clear. In particular, a standalone
`ckati` executable needs accompanying licence and notice files; embedding them
in the executable is not required by this repository's distribution layout.

The existing Dockerfile uses `COPY . .`, and `.dockerignore` does not exclude
these files, so the test image includes them under `/workspace`. A future
minimal runtime image or binary archive must copy them explicitly. External
runtime libraries and base-image components retain their own distribution
requirements.

## Contributions

Submit original contributions under PolyForm Perimeter 1.0.1 unless a different
licence is explicitly identified and accepted. Only contribute material you
have the right to license. Preserve upstream copyright, licence, patent,
trademark, and attribution notices; do not replace an upstream licence with the
fork's default. Add or update a prominent change notice when modifying an
upstream-derived file, and keep the scope of mixed-file licensing clear.

For any future format that cannot carry comments, include an adjacent notice
listing the exact path, provenance, and changes, and document the exception
here. Do not alter parser-sensitive fixture bytes merely to insert a header.
Review new third-party imports and include their applicable licences and
notices. Update the provenance inventory when carrying out a new licensing
audit, recording its upstream and fork revisions.

## Licence text sources

- [Official PolyForm Perimeter 1.0.1 text](https://github.com/polyformproject/polyform-licenses/blob/1.0.0/PolyForm-Perimeter-1.0.1.md), Git blob `088231fb5ec6ed13bdcd3e67f4c2b0f404eb0568`.
- [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0), preserved from this repository's original `LICENSE` without editing.
