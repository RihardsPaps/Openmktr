<!-- Copyright 2026 Rihards Paps and Haralds Paps; SPDX-License-Identifier: MPL-2.0 -->

# Cross-version compiler-cache investigation

This external investigation closed at its qualification gate on 24 October
2026. It adds no Openmktr runtime option or artifact format.

Portage normally builds each package version in a different working directory.
The [recursive no-op cache](recursive-cache.md) validates an existing directory;
[persistent workspaces](workspaces.md) instead retain stable paths and build
state. This investigation tested ccache reuse between fresh version directories.

## Outcome

The conservative ccache configuration produced correct outputs but increased
median full-build time by 4.3% to 12.5% in two small package upgrades.
All 102 controlled combinations and 120 measured new-version builds passed,
but the results did not justify a custom artifact cache.

A bounded xz 5.8.3-to-5.8.4 path probe used ccache 4.14 and three corresponding
liblzma compilations, with and without dependency generation. All 18 real-upgrade
comparisons produced zero cross-version hits. Another 18 relocation-only
comparisons also produced zero hits. Same-directory positive controls hit and
matched their seed objects, diagnostics and dependencies.

Leaving `CCACHE_BASEDIR` unset preserved all six tested object matches; using
a common parent or each version's work directory preserved none. Dependency
coverage matched in all three applicable cases per profile. Relative path
rewriting retained version-specific path components and could alter object
content. No profile qualified for further investigation; the conditional
compilation-heavy C++ upgrade was not run.

## Evidence and limits

[Full-build results](cross-version-reuse-results.json) and
[path-probe results](cross-version-path-results.json) retain samples,
configurations, binary identities, commands and evidence references.
The [external compatibility guide](../validation/compatibility/README.md#cross-version-compiler-cache-investigation)
describes the campaign tools. The portable accounting suite is
`python tests/cross_version_cache.py`; it requires neither ccache nor Portage.

The original raw run inherited an incorrect `openmktr_use_test=true` marker
from a shared helper. It does not establish an ebuild installation test.
The checked-in results correct the marker to false with an explicit erratum.
Supplemental positive-hit and compile-inventory checks passed; original timings
and archived source hashes remain unchanged.

These observations apply to the recorded packages, paths and cache policy.
They do not establish the performance or safety of other cache configurations.
