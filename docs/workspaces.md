# Persistent incremental workspaces

`omktr workspace` keeps a stable source/build location across release-directory
changes. Unchanged source bytes retain their workspace timestamps, so Ninja can
reuse objects and its dependency/build logs. Changed sources, headers and recipes
still rebuild. Python 3.14 or newer, including its standard-library `lzma` module,
is required for these optional commands.

## Build and upgrade

For a prepared source tree with a Makefile:

```sh
omktr workspace run --name my-project --source /sources/project-1.0
omktr workspace run --name my-project
omktr workspace run --name my-project --source /sources/project-1.1
```

The first command synchronizes the source into the managed workspace and runs
`omktr build`. The second runs incrementally in the existing workspace. The third
synchronizes the new prepared release into the same location, retaining unchanged
inputs and outputs. The original source directories are not modified. Use the
same name across versions; the name is a project identity supplied by the caller.

Additional build arguments or commands go after `--`:

```sh
omktr workspace run --name my-project -- omktr build -j16 all
omktr workspace path --name my-project
omktr workspace status --name my-project
```

`path` restores a packed workspace before printing its working directory.
`status` inspects metadata without restoring it. Always run managed builds through
`workspace run` so that the ownership lock also protects them from compression.

For an out-of-tree configure build:

```sh
omktr workspace run --name my-project --source /sources/project-1.0 \
  --out-of-tree \
  --configure '"$OMKTR_WORKSPACE_SOURCE/configure" --enable-dependency-tracking'
```

The configure command runs in the stable build directory, then `omktr build`
runs there. Configuration runs again when prepared source contents or the
configure command change. An unchanged configuration is reused. The selected
layout and configure command are retained for subsequent runs.

`OMKTR_WORKSPACE_SOURCE` and `OMKTR_WORKSPACE_BUILD` identify the stable directories
inside commands. `OMKTR_WORKSPACE_TOOL` identifies the calling Openmktr executable.
`--configure` is an explicit POSIX shell command; commands after `--` are passed
as an argument vector. Exit statuses and cancellation are forwarded.

## Configuration and dependency correctness

Workspace names exclude versions. Separate profiles isolate compiler executable
identities, compiler/linker flags, architecture, sysroot, USE/ABI settings and an
optional `--context` value. Switching back to an earlier configuration selects
its retained profile. A newly selected configuration needs a prepared `--source`
tree. Compiler and source identities include ctime, so preserving only mtime does
not hide a content change.

Set `CC` and `CXX` to the actual literal compiler invocations; the defaults are
`clang` and `clang++`; both selected commands must be identifiable. For dependencies outside the prepared tree, SDK contents,
auxiliary tools, or compiler choices overridden inside a Makefile, use complete
build prerequisites and a `--context` value that changes with those inputs.
The standalone frontend cannot infer arbitrary external dependency changes.
Portage additionally keys profiles on installed build-dependency identities.

Source synchronization copies changed files, removes inputs removed from the
prepared tree, preserves unchanged file timestamps, and advances changed inputs
beyond existing workspace timestamps. It leaves generated files available for
incremental builds. Managed source copies are build state: make source edits in
the supplied source tree and synchronize it again.
Relative input ages are retained, so current distributed generated sources do
not spuriously regenerate. When an incoming regular file is unchanged, derived
changes made to its workspace copy by the build are retained. Directory mode
changes preserve generated files inside that directory.
Adding, removing or retargeting source symlinks starts a clean build at the same
workspace paths. This also covers directory and external links: their referents
can be older than retained outputs, and external timestamps are left intact.
Unchanged links retain incremental state, including after compression/restoration.

Build rules must describe their inputs, including generated headers and compiler
dependencies. Use explicit prerequisites or `.KATI_DEPFILE`, and enable Automake
dependency tracking. Scheduled workspace recipes with depfiles follow Ninja's
complete dependency information. Workspace recipe identities also ensure that a
changed recipe can rebuild an existing output even when declared prerequisites
are older. Identities are compared as full bytes and updated only on successful
execution; storage filename hashes are not used as equality proofs.

The workspace root defaults to `${XDG_CACHE_HOME:-$HOME/.cache}/openmktr/workspaces`.
`--root DIR` or `OMKTR_WORKSPACE_ROOT` selects another cache. Sources and the
managed cache must not overlap. Cache metadata belongs to its build user.
Cache roots must not be writable by other users. Package and profile metadata
directories are private, including when build commands use a permissive umask.

If source synchronization fails or is interrupted, builds and packing stop until
you retry with `--source`. `status` shows the pending synchronization. Retrying
can use another prepared release: partially introduced inputs are tracked and
removed when absent there. Copying uses private staging outside the build tree,
so interrupted temporary files cannot become accidental build inputs.
Recovery rebases all regular input timestamps and retries configuration, allowing
a conservative rebuild after the incomplete synchronization.

## Compress inactive workspaces

Active workspaces stay expanded for fast incremental runs. Pack an inactive
workspace, including its source, generated files, objects and Ninja state:

```sh
omktr workspace pack --name my-project
omktr workspace status --name my-project
omktr workspace run --name my-project
```

Packing defaults to XZ and reports original and compressed byte counts and the
selected format. Use `omktr workspace pack --name my-project --compression=zstd`
for Zstandard level 3, or `--compression=xz` for XZ preset 6. The same option
applies to `gc`. Zstandard uses Python's optional `compression.zstd` module when
available. Distributions that omit it, including stable Alpine, can provide the
distribution `zstd` executable instead. That backend uses an unnamed temporary
tar file, so it needs additional temporary disk space. Both backends read the
same archives and retain checksum, restoration and atomic publication checks.
The reference image checks both standard-library compression modules.

The archive format is recorded with the workspace; archives without that field
are restored as legacy XZ workspaces. Existing archives are converted only when
you restore and explicitly repack them. An unknown format fails without replacing
expanded files. The mixed source/object measurement in
[compression-results.json](../validation/compression-results.json) found Zstandard
archives about 38% larger. This exceeds the 25% default-selection limit, so XZ
remains the default despite faster packing and restoration in that workload.
The next `run` automatically restores the workspace at the same absolute paths.
File modes, symlinks and nanosecond timestamps are restored, allowing existing
objects and Ninja dependency logs to remain useful. Compression and restoration
add cold-start work; packing is an explicit maintenance operation rather than a
step on every hot no-op build.

To pack all eligible profiles that have been unused for an hour:

```sh
omktr workspace gc --idle-seconds 3600
```

`gc` skips locked builds and active Portage owners. It compresses retained state
without evicting it. `unpack --name NAME` explicitly restores a workspace.
Portage hooks refresh the idle age on phase entry and installation completion,
so a long successful build is not treated as idle immediately after it finishes.

Every archived file is read back and verified before expanded files are removed.
The archive is published atomically and checksummed; restoration validates its
inventory, file contents and paths in a private staging directory before
publishing the expanded tree. Corruption fails visibly and retains the archive.
Archives cannot write through symlink parents or paths outside the workspace.
Hard-linked outputs and special files are rejected without removing the expanded
workspace; compression currently supports regular files, directories and symlinks.

## Opt-in Portage profile

The supplied profile targets native EAPI 8 Make-based packages whose build phases
use `S` and `B`. Portage still unpacks and prepares each version normally; a
post-prepare hook synchronizes those prepared bytes into a version-independent
workspace before configuration. Configuration, tests, staged installation and
merge retain their normal Portage lifecycle. `WORKDIR` and `${D}` are unchanged.
See the [Gentoo variable reference](https://devmanual.gentoo.org/ebuild-writing/variables/index.html)
for their roles.

Create a cache owned by the Portage build user, then select the installed
`portage/examples/openmktr-workspace` environment for individual packages through
`package.env`, as described in the [adapter guide](../packaging/gentoo/README.md).
Merge the accompanying `openmktr-workspace.bashrc` snippet into
`/etc/portage/bashrc` to load phase hooks for selected packages. Environment
files contain variable settings; Portage's shell hooks load the functions.
The example enables Ninja compilation and Automake dependency tracking. Known
disabled Automake tracking is rejected before compilation.

The key uses category, package name, slot and target, plus configuration/compiler
and installed dependency identities. `PV`, `PVR`, `PF`, and the versioned
`WORKDIR` path do not select a new workspace. Source preparation and configuration
still apply the new version's actual source and settings. Active ownership is
checked against Portage's build-directory lock, including before each subsequent
source phase; another version cannot silently replace an active build's sources.

Existing custom phase hooks must be merged with the example hooks. Packages
using additional versioned build paths, hard-coded `WORKDIR` paths, custom
lifecycle cleanup, or incomplete dependencies need package-specific qualification.
The integration remains opt-in; it does not enable packages or edit host Portage
configuration during installation.

After Portage finishes, idle cache maintenance can run as the cache owner:

```sh
omktr workspace gc --root /var/cache/openmktr/workspaces --idle-seconds 3600
```

Maintenance invoked by root for a cache owned by the build user drops privileges
to that owner. New builds must run as the cache owner. Unselecting the environment
restores the package's normal workspace lifecycle; retained state stays available
until explicitly removed by its owner.
