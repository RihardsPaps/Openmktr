#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Persistent, version-independent build workspaces and idle compression."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time


SCHEMA = 1
DEFAULT_COMPRESSION = "xz"
ARCHIVES = {"xz": "content.tar.xz", "zstd": "content.tar.zst"}
CONFIG_ENV = ("CC", "CXX", "CPP", "LD", "AR", "AS", "NM", "RANLIB",
              "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CHOST", "CBUILD",
              "CTARGET", "SYSROOT", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR",
              "USE", "ABI", "LLVM", "LLVM_IAS", "ARCH", "CROSS_COMPILE")


class WorkspaceBusy(RuntimeError):
    pass


class WorkspaceCommandError(RuntimeError):
    def __init__(self, status):
        super().__init__(f"workspace command failed with status {status}")
        self.status = status


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    fd, name = tempfile.mkstemp(prefix=".state-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded(value) + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def read_json(path):
    return json.loads(path.read_text())


def identity(path):
    s = path.lstat()
    return [s.st_dev, s.st_ino, s.st_mode, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


def remove_owned(path, owner):
    if path == owner or not path.resolve().is_relative_to(owner.resolve()):
        # A symlink is removed as a link, never followed out of the workspace.
        if path.is_symlink() and path.parent.resolve().is_relative_to(owner.resolve()):
            path.unlink()
            return
        raise RuntimeError(f"refusing removal outside managed workspace: {path}")
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


@contextmanager
def locked(path, nonblocking=False):
    with path.open("a+b") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0))
        yield


@contextmanager
def synchronization_staging(workspace):
    staging = workspace / ".sync"
    if staging.exists() or staging.is_symlink():
        remove_owned(staging, workspace)
    staging.mkdir()
    try:
        yield
    finally:
        if staging.exists() or staging.is_symlink():
            remove_owned(staging, workspace)


def require_synchronized(state):
    if state.get("sync_pending"):
        raise RuntimeError("source synchronization incomplete; rerun with --source before building or packing")


def private_metadata_directory(path):
    if path.is_symlink():
        raise RuntimeError("workspace metadata directory must not be a symlink")
    info = path.stat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or
            stat.S_IMODE(info.st_mode) & 0o022):
        raise RuntimeError("workspace metadata must belong to the build user and not be writable by other users")
    # Child commands may choose a permissive umask; keep their data behind
    # a private parent even when this is an existing cache.
    path.chmod(0o700)


def portage_lock(builddir):
    path = Path(builddir)
    return path.parent / ("." + path.name + ".portage_lockfile")


def owner_busy(path):
    """Portage uses lockf, with flock as its documented platform fallback."""
    try:
        stream = Path(path).open("rb")
    except FileNotFoundError:
        return False
    with stream:
        for lock in (fcntl.lockf, fcntl.flock):
            try:
                lock(stream, fcntl.LOCK_SH | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
    return False


def check_owner(state, owner=None):
    previous = state.get("owner")
    if previous and previous != owner and owner_busy(previous):
        raise WorkspaceBusy(f"workspace belongs to an active Portage build: {previous}")


def tree(root):
    result = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in sorted(dirs + files):
            path = parent / name
            relative = path.relative_to(root).as_posix()
            if "\n" in relative or "\r" in relative:
                raise RuntimeError("workspace input names cannot contain newlines")
            s = path.lstat()
            if stat.S_ISLNK(s.st_mode):
                record = dict(kind="link", target=os.readlink(path))
            elif stat.S_ISDIR(s.st_mode):
                record = dict(kind="dir")
            elif stat.S_ISREG(s.st_mode):
                record = dict(kind="file", links=s.st_nlink)
            else:
                raise RuntimeError(f"cannot retain special file: {path}")
            record.update(mode=stat.S_IMODE(s.st_mode), mtime_ns=s.st_mtime_ns,
                          identity=identity(path))
            result[relative] = record
    return result


def synchronize(source, destination, state, workspace, force=False):
    source = source.resolve(strict=True)
    if workspace.resolve().is_relative_to(source) or source.is_relative_to(workspace.resolve()):
        raise RuntimeError("source and managed workspace must not overlap")
    if not source.is_dir():
        raise RuntimeError("--source must be a prepared source directory")
    destination.mkdir(parents=True, exist_ok=True)
    records = tree(source)
    manifest_path = workspace / "inputs.json"
    old = read_json(manifest_path) if manifest_path.exists() else {}
    old_links = {name: item["target"] for name, item in old.items() if item["kind"] == "link"}
    new_links = {name: item["target"] for name, item in records.items() if item["kind"] == "link"}
    if manifest_path.exists() and old_links != new_links:
        # Make and Ninja stat the referent, not the link. A new referent may
        # predate every retained output, even for links to whole directories
        # or external files whose timestamps we must not modify. Start cold
        # at the same paths instead of silently reusing those outputs.
        remove_owned(workspace / "content", workspace)
        destination.mkdir(parents=True)
        (workspace / "content/build").mkdir()
        state.pop("configured", None)
    # Record newly introduced input names before publishing any of them.
    # After interruption, a different prepared release can then remove those
    # inputs instead of mistaking them for retained generated outputs.
    atomic_json(manifest_path, {**{k: v for k, v in records.items() if k not in old}, **old})
    changed = []
    newest = None
    timestamp_ranks = {}
    for name in sorted(old.keys() - records.keys(), key=lambda p: (-p.count("/"), p)):
        target = destination / name
        if old[name]["kind"] == "dir" and target.is_dir() and not target.is_symlink():
            if not target.resolve().is_relative_to(workspace.resolve()):
                raise RuntimeError(f"refusing removal outside managed workspace: {target}")
            try:
                target.rmdir()
            except OSError:
                pass  # Retain unrelated generated files inside old directories.
        else:
            remove_owned(target, workspace)
        changed.append(name)
    for name, item in sorted(records.items(), key=lambda p: (p[0].count("/"), p[0])):
        origin, target = source / name, destination / name
        previous = old.get(name, {})
        if item["kind"] == "file":
            same_origin = (previous.get("identity") == item["identity"] and
                           previous.get("origin") == str(source))
            item["sha256"] = previous["sha256"] if same_origin else digest(origin)
        same = False
        retain = False
        if target.exists() or target.is_symlink():
            s = target.lstat()
            if item["kind"] == "dir":
                same = stat.S_ISDIR(s.st_mode)
                retain = same
            elif item["kind"] == "link":
                same = stat.S_ISLNK(s.st_mode) and os.readlink(target) == item["target"]
            elif stat.S_ISREG(s.st_mode):
                # A build may regenerate a distributed source or configure
                # an input in place. An unchanged incoming file must not
                # overwrite that derived state on the next synchronization.
                retain = (not force and previous.get("kind") == "file" and
                          previous.get("sha256") == item["sha256"])
                if retain:
                    same = True
                elif not force:
                    same_destination = previous.get("destination_identity") == identity(target)
                    current = previous.get("sha256") if same_destination else digest(target)
                    same = current == item["sha256"]
            if retain:
                if ((force or previous.get("mode") != item["mode"]) and
                        stat.S_IMODE(s.st_mode) != item["mode"]):
                    os.chmod(target, item["mode"])
                    changed.append(name)
            else:
                same = same and stat.S_IMODE(s.st_mode) == item["mode"]
        if not same:
            if target.exists() or target.is_symlink():
                remove_owned(target, workspace)
            target.parent.mkdir(parents=True, exist_ok=True)
            if item["kind"] == "dir":
                target.mkdir()
            elif item["kind"] == "link":
                target.symlink_to(item["target"])
            else:
                if newest is None:
                    newest = 1000 + max([time.time_ns()] + [r["mtime_ns"] for r in tree(workspace / "content").values()])
                    timestamp_ranks = {value: rank for rank, value in enumerate(sorted({
                        record["mtime_ns"] for record in records.values() if record["kind"] == "file"}))}
                # Preserve input age ordering and ties, including distributed
                # generated sources. Microseconds remain distinct to the
                # direct executor's floating-point timestamp comparisons.
                stamp = newest + 1000 * timestamp_ranks[item["mtime_ns"]]
                fd, temporary = tempfile.mkstemp(prefix=".input-", dir=workspace / ".sync")
                try:
                    with os.fdopen(fd, "wb") as out, origin.open("rb") as incoming:
                        shutil.copyfileobj(incoming, out)
                    os.chmod(temporary, item["mode"])
                    os.utime(temporary, ns=(stamp, stamp))
                    os.replace(temporary, target)
                finally:
                    Path(temporary).unlink(missing_ok=True)
            if item["kind"] == "dir":
                os.chmod(target, item["mode"])
            changed.append(name)
        item["origin"] = str(source)
        item["destination_identity"] = identity(target)
    atomic_json(manifest_path, records)
    state["source_digest"] = hashlib.sha256(encoded({
        k: {f: v[f] for f in ("kind", "mode", "sha256", "target") if f in v}
        for k, v in records.items()})).hexdigest()
    state["source"] = str(source)
    state["input_count"] = len(records)
    return changed


def has_zstd_module():
    try:
        from compression import zstd
        return True
    except ImportError:
        return False


@contextmanager
def workspace_archive(path, compression=None):
    writing = compression is not None
    if not writing:
        with path.open("rb") as stream:
            compression = "zstd" if stream.read(4) == b"\x28\xb5\x2f\xfd" else "xz"
    if compression != "zstd" or has_zstd_module():
        options = dict(format=tarfile.PAX_FORMAT) if writing else {}
        if writing:
            options.update(dict(preset=6) if compression == "xz" else dict(level=3))
        mode = ("w:xz" if compression == "xz" else "w:zst") if writing else "r:*"
        with tarfile.open(path, mode, **options) as archive:
            yield archive
        return
    # Alpine's stable Python omits _zstd. Keep the same archive format through
    # its distribution compressor; the reference path still uses Python's module.
    compressor = shutil.which("zstd")
    if not compressor:
        raise RuntimeError("Zstandard requires Python's compression.zstd module or the zstd command")
    # A private, unnamed spool avoids pipe deadlocks and is removed on signals
    # and exceptions. Verification and publication remain in the caller.
    with tempfile.TemporaryFile(dir=path.parent) as spool:
        if writing:
            with tarfile.open(fileobj=spool, mode="w:", format=tarfile.PAX_FORMAT) as archive:
                yield archive
            spool.seek(0)
            with path.open("wb") as output:
                status = subprocess.run([compressor, "-q", "-3", "-c"], stdin=spool,
                                        stdout=output, stderr=subprocess.PIPE)
        else:
            with path.open("rb") as incoming:
                status = subprocess.run([compressor, "-q", "-d", "-c"], stdin=incoming,
                                        stdout=spool, stderr=subprocess.PIPE)
            if status.returncode == 0:
                spool.seek(0)
                with tarfile.open(fileobj=spool, mode="r:") as archive:
                    yield archive
        if status.returncode:
            raise RuntimeError("Zstandard command failed: " + status.stderr.decode(errors="replace").strip())


def pack(workspace, state, compression=DEFAULT_COMPRESSION):
    check_owner(state)
    require_synchronized(state)
    if state.get("packed"):
        stored_format = state.get("archive_format", "xz")
        if stored_format not in ARCHIVES:
            raise RuntimeError("unsupported workspace archive format")
        return dict(already_packed=True, compression=stored_format)
    content = workspace / "content"
    entries = tree(content)
    if any(item.get("links", 1) > 1 for item in entries.values()):
        raise RuntimeError("cannot compress hard-linked files without changing their inode relationships")
    fd, temporary = tempfile.mkstemp(prefix=".archive-", dir=workspace)
    os.close(fd)
    original_bytes = sum(v["identity"][3] for v in entries.values() if v["kind"] == "file")
    try:
        with workspace_archive(Path(temporary), compression) as archive:
            for name, item in sorted(entries.items()):
                path = content / name
                member = tarfile.TarInfo("payload/" + name)
                member.mode = item["mode"]
                member.pax_headers["mtime"] = f"{item['mtime_ns'] // 10**9}.{item['mtime_ns'] % 10**9:09d}"
                if item["kind"] == "dir":
                    member.type = tarfile.DIRTYPE
                    archive.addfile(member)
                elif item["kind"] == "link":
                    member.type = tarfile.SYMTYPE
                    member.linkname = item["target"]
                    archive.addfile(member)
                else:
                    member.size = item["identity"][3]
                    item["sha256"] = digest(path)
                    with path.open("rb") as stream:
                        archive.addfile(member, stream)
            input_path = workspace / "inputs.json"
            manifest = encoded(dict(entries=entries, inputs=read_json(input_path) if input_path.exists() else {}))
            member = tarfile.TarInfo("manifest.json")
            member.size = len(manifest)
            archive.addfile(member, io.BytesIO(manifest))
        # Read every archived byte back before removing the expanded tree.
        # A concurrent external write or an interrupted archive cannot destroy
        # the only usable copy of the workspace.
        restore_archive(Path(temporary))
        archive_path = workspace / ARCHIVES[compression]
        checksum = digest(Path(temporary))
        os.replace(temporary, archive_path)
        state.update(packed=True, archive_sha256=checksum, archive_format=compression)
        atomic_json(workspace / "state.json", state)
        detached = workspace / ".packed-content"
        if detached.exists():
            remove_owned(detached, workspace)
        content.rename(detached)
        # Portage checks S before pre-phase hooks can restore its contents.
        (content / "source").mkdir(parents=True)
        remove_owned(detached, workspace)
        (workspace / "inputs.json").unlink(missing_ok=True)
        return dict(original_bytes=original_bytes, compressed_bytes=archive_path.stat().st_size,
                    compression=compression)
    finally:
        Path(temporary).unlink(missing_ok=True)


def restore_archive(archive_path, staging=None):
    # No tarfile extraction API is used: parent types are checked and links
    # are created last, so neither absolute links nor .. can redirect writes.
    with workspace_archive(archive_path) as archive:
        inventory = archive.getmembers()
        if sum(m.name == "manifest.json" for m in inventory) != 1:
            raise RuntimeError("workspace archive is missing its manifest")
        with archive.extractfile("manifest.json") as stream:
            manifest = json.load(stream)
        entries = manifest["entries"]
        members = [m for m in inventory if m.name != "manifest.json"]
        if len(members) != len(entries) or len({m.name for m in members}) != len(entries):
            raise RuntimeError("workspace archive inventory mismatch")
        for member in members:
            if not member.name.startswith("payload/"):
                raise RuntimeError("unsafe workspace archive namespace")
            member.name = member.name[len("payload/"):]
            name = PurePosixPath(member.name)
            item = entries.get(member.name)
            if (name.is_absolute() or ".." in name.parts or not item or
                    not member.name or member.name != str(name)):
                raise RuntimeError("unsafe workspace archive path")
            for parent in name.parents:
                if str(parent) != "." and entries.get(str(parent), {}).get("kind") != "dir":
                    raise RuntimeError("workspace archive has a non-directory parent")
            expected = {"dir": member.isdir(), "file": member.isfile(), "link": member.issym()}
            if not expected[item["kind"]]:
                raise RuntimeError("workspace archive type mismatch")
            target = staging / member.name if staging else None
            if member.isdir():
                if target:
                    target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                if member.size != item["identity"][3]:
                    raise RuntimeError("workspace archive size mismatch")
                with archive.extractfile(member) as incoming:
                    if target:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with target.open("xb") as out:
                            shutil.copyfileobj(incoming, out)
                        checksum = digest(target)
                    else:
                        checksum = hashlib.file_digest(incoming, "sha256").hexdigest()
                if checksum != item["sha256"]:
                    raise RuntimeError("workspace restored file checksum mismatch")
        for member in members:
            item = entries[member.name]
            target = staging / member.name if staging else None
            if member.issym():
                if member.linkname != item["target"]:
                    raise RuntimeError("workspace archive link mismatch")
                if target:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.symlink_to(item["target"])
            elif target:
                os.chmod(target, item["mode"])
        if staging:
            for name, item in sorted(entries.items(), key=lambda p: -p[0].count("/")):
                os.utime(staging / name, ns=(item["mtime_ns"], item["mtime_ns"]), follow_symlinks=False)
        return manifest


def unpack(workspace, state):
    if not state.get("packed"):
        return
    compression = state.get("archive_format", "xz")
    if compression not in ARCHIVES:
        raise RuntimeError("unsupported workspace archive format")
    archive_path = workspace / ARCHIVES[compression]
    if digest(archive_path) != state["archive_sha256"]:
        raise RuntimeError("workspace archive checksum mismatch; expanded files were not replaced")
    staging = Path(tempfile.mkdtemp(prefix=".restore-", dir=workspace))
    try:
        manifest = restore_archive(archive_path, staging)
        content = workspace / "content"
        if content.exists():
            remove_owned(content, workspace)
        staging.rename(content)
        state["packed"] = False
        atomic_json(workspace / "inputs.json", manifest["inputs"])
        state["last_used"] = time.time()
        state.pop("archive_sha256", None)
        state.pop("archive_format", None)
        atomic_json(workspace / "state.json", state)
        archive_path.unlink()
        detached = workspace / ".packed-content"
        if detached.exists():
            remove_owned(detached, workspace)
    finally:
        if staging.exists():
            remove_owned(staging, workspace)


def package_base(root, name):
    if not name or "\n" in name or "\r" in name:
        raise RuntimeError("workspace name must be nonempty and contain no newlines")
    key = hashlib.sha256(name.encode()).hexdigest()[:24]
    base = root / key
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    private_metadata_directory(base)
    return base


def index_for(base, name):
    path = base / "index.json"
    if path.exists():
        index = read_json(path)
        if index.get("schema") != SCHEMA or index.get("name") != name:
            raise RuntimeError("workspace identity/schema mismatch")
        return index
    if any(p.name != ".lock" for p in base.iterdir()):
        raise RuntimeError("refusing to adopt an unrecognised workspace directory")
    index = dict(schema=SCHEMA, name=name, profiles=[])
    atomic_json(path, index)
    return index


def configuration(base, context, out_of_tree):
    cache_path = base / "compilers.json"
    try:
        cache = read_json(cache_path)
    except (FileNotFoundError, ValueError):
        cache = {}
    compilers = {}
    for variable, default in (("CC", "clang"), ("CXX", "clang++")):
        words = shlex.split(os.environ.get(variable) or default)
        if not words or any(x in words for x in (";", "|", "&&")):
            raise RuntimeError(f"{variable} must name a literal compiler invocation")
        files = []
        for word in words:
            found = shutil.which(word)
            if found and not word.startswith("-"):
                path = Path(found).resolve()
                stamp = identity(path)
                previous = cache.get(str(path), {})
                checksum = previous.get("sha256") if previous.get("identity") == stamp else digest(path)
                cache[str(path)] = dict(identity=stamp, sha256=checksum)
                files.append(dict(path=str(path), sha256=checksum))
        if not files:
            raise RuntimeError(f"cannot identify {variable} compiler: {words[0]}")
        compilers[variable] = dict(arguments=words, files=files)
    atomic_json(cache_path, cache)
    return dict(environment={k: os.environ.get(k, "") for k in CONFIG_ENV},
                compilers=compilers, context=context, out_of_tree=out_of_tree)


def select_profile(base, index, context, out_of_tree):
    key = hashlib.sha256(encoded(context)).hexdigest()[:24]
    workspace = base / key
    if workspace.is_symlink():
        raise RuntimeError("workspace profile must not be a symlink")
    workspace.mkdir(mode=0o700, exist_ok=True)
    private_metadata_directory(workspace)
    path = workspace / "state.json"
    if path.exists():
        state = read_json(path)
        if state.get("schema") != SCHEMA or state.get("configuration") != context:
            raise RuntimeError("workspace profile configuration mismatch")
    else:
        if any(workspace.iterdir()):
            raise RuntimeError("refusing to adopt unrecognised build files")
        state = dict(schema=SCHEMA, configuration=context, out_of_tree=out_of_tree,
                     packed=False, last_used=time.time())
        atomic_json(path, state)
        (workspace / "content/source").mkdir(parents=True)
        (workspace / "content/build").mkdir()
    index["current"] = key
    index["out_of_tree"] = out_of_tree
    index["context"] = context["context"]["user"]
    if key not in index["profiles"]:
        index["profiles"].append(key)
    atomic_json(base / "index.json", index)
    return workspace, state


def current_profile(base, index):
    key = index.get("current", "")
    if key not in index["profiles"] or len(key) != 24 or any(c not in "0123456789abcdef" for c in key):
        raise RuntimeError("workspace has no selected configuration; run with --source first")
    workspace = base / key
    if workspace.is_symlink():
        raise RuntimeError("workspace profile must not be a symlink")
    private_metadata_directory(workspace)
    state = read_json(workspace / "state.json")
    if state.get("schema") != SCHEMA:
        raise RuntimeError("unsupported workspace schema")
    return workspace, state


def paths(workspace, state):
    source = workspace / "content/source"
    return source, workspace / "content/build" if state["out_of_tree"] else source


def run_child(command, cwd, env):
    process = None
    interrupted = 0

    def forward(sig, frame):
        nonlocal interrupted
        interrupted = sig
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass

    previous = {s: signal.signal(s, forward) for s in
                (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM)}
    try:
        process = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True)
        if interrupted:
            forward(interrupted, None)
        code = process.wait()
        code = 128 + interrupted if interrupted else 128 - code if code < 0 else code
        if code:
            raise WorkspaceCommandError(code)
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def portage_context():
    required = ("CATEGORY", "PN", "SLOT", "CHOST", "S", "WORKDIR", "PORTAGE_BUILDDIR")
    if any(not os.environ.get(k) for k in required):
        raise RuntimeError("Portage workspace requires a native EAPI 8 source build")
    if os.environ.get("EAPI") != "8" or os.environ.get("EPREFIX", ""):
        raise RuntimeError("Portage workspace currently supports EAPI 8 without Prefix")
    if os.environ.get("CBUILD", os.environ["CHOST"]) != os.environ["CHOST"]:
        raise RuntimeError("Portage workspace currently supports native builds")
    owner = str(portage_lock(os.environ["PORTAGE_BUILDDIR"]))
    if not owner_busy(owner):
        raise RuntimeError("Portage build-directory lock is not held")
    name = "/".join((os.environ["CATEGORY"], os.environ["PN"])) + ":" + os.environ["SLOT"] + ":" + os.environ["CHOST"]
    # Installed dependency identities isolate ABI/configuration changes without
    # keying the workspace on the package's own PV/PVR/PF or WORKDIR pathname.
    dependencies = {}
    expressions = {k: os.environ.get(k, "") for k in ("DEPEND", "BDEPEND")}
    if any(expressions.values()):
        import portage
        db = portage.db[portage.root]["vartree"].dbapi
        for expression in expressions.values():
            atoms = portage.dep.use_reduce(expression, uselist=os.environ.get("USE", "").split(),
                                           flat=True, token_class=portage.dep.Atom)
            for atom in atoms:
                if isinstance(atom, portage.dep.Atom) and not atom.blocker:
                    for cpv in db.match(atom):
                        dependencies[cpv] = db.aux_get(cpv, ["SLOT", "USE", "COUNTER"])
    return name, owner, dependencies


def main():
    if sys.version_info < (3, 14):
        raise RuntimeError("workspace commands require Python 3.14 or newer")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("run", "path", "status", "pack", "unpack", "gc",
                                           "portage-prepare", "portage-enter"))
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("OMKTR_WORKSPACE_ROOT",
                        str(Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "openmktr/workspaces"))))
    parser.add_argument("--name")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--context")
    layout = parser.add_mutually_exclusive_group()
    layout.add_argument("--out-of-tree", dest="out_of_tree", action="store_true", default=None)
    layout.add_argument("--in-source", dest="out_of_tree", action="store_false")
    parser.add_argument("--configure", help="explicit POSIX shell command; workspace paths are exported")
    parser.add_argument("--idle-seconds", type=float, default=3600)
    parser.add_argument("--compression", choices=tuple(ARCHIVES), default=None,
                        help="packing format for pack/gc (default: " + DEFAULT_COMPRESSION + ")")
    # REMAINDER is split explicitly so workspace options remain ordinary options.
    arguments = sys.argv[1:]
    command = []
    if "--" in arguments:
        split = arguments.index("--")
        arguments, command = arguments[:split], arguments[split + 1:]
    args = parser.parse_args(arguments)
    if args.compression is not None and args.action not in ("pack", "gc"):
        parser.error("--compression applies only to pack and gc")
    args.compression = args.compression or DEFAULT_COMPRESSION
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cache_owner = root.stat()
    if os.geteuid() == 0 and cache_owner.st_uid != 0:
        if args.action in ("run", "portage-prepare"):
            raise RuntimeError("run builds as the workspace cache owner")
        # Maintenance must not turn a build user's cached data into root-owned
        # files, or extract user-controlled data with root privileges.
        os.setgroups([])
        os.setgid(cache_owner.st_gid)
        os.setuid(cache_owner.st_uid)
    elif cache_owner.st_uid != os.geteuid():
        raise RuntimeError("workspace cache must belong to the current build user")
    if stat.S_IMODE(cache_owner.st_mode) & 0o022:
        raise RuntimeError("workspace cache must not be writable by other users")
    if args.action == "gc":
        count = 0
        for marker in sorted(root.glob("*/index.json")):
            index = read_json(marker)
            base = package_base(root, index["name"])
            if base != marker.parent:
                raise RuntimeError("workspace index path mismatch")
            try:
                with locked(base / ".lock", nonblocking=True):
                    for key in index["profiles"]:
                        workspace, state = current_profile(base, {**index, "current": key})
                        if not state.get("packed") and time.time() - state["last_used"] >= args.idle_seconds:
                            try:
                                pack(workspace, state, args.compression)
                                count += 1
                            except WorkspaceBusy:
                                pass
            except BlockingIOError:
                pass
        print(json.dumps(dict(packed_workspaces=count)))
        return
    owner, dependencies = None, {}
    if args.action.startswith("portage-"):
        args.name, owner, dependencies = portage_context()
    if not args.name:
        parser.error("--name is required")
    base = package_base(root, args.name)
    with locked(base / ".lock"):
        index = index_for(base, args.name)
        if args.action in ("run", "portage-prepare"):
            if args.context is None:
                args.context = index.get("context", "")
            if args.action == "portage-prepare":
                args.source = Path(os.environ["S"])
                original_build = Path(os.environ.get("B") or os.environ["S"])
                args.out_of_tree = original_build.resolve() != args.source.resolve()
                if (not args.source.resolve().is_relative_to(Path(os.environ["WORKDIR"]).resolve()) or
                        not original_build.resolve().is_relative_to(Path(os.environ["WORKDIR"]).resolve())):
                    raise RuntimeError("Portage source/build directories must be inside WORKDIR")
            if args.out_of_tree is None:
                args.out_of_tree = index.get("out_of_tree", False)
            context = configuration(base, dict(user=args.context, dependencies=dependencies), args.out_of_tree)
            workspace, state = select_profile(base, index, context, args.out_of_tree)
            check_owner(state, owner)
            unpack(workspace, state)
            if args.source:
                recovering = bool(state.get("sync_pending"))
                if recovering:
                    state.pop("configured", None)
                state["sync_pending"] = str(args.source.resolve())
                atomic_json(workspace / "state.json", state)
                with synchronization_staging(workspace):
                    synchronize(args.source, workspace / "content/source", state, workspace, force=recovering)
                state.pop("sync_pending", None)
            else:
                require_synchronized(state)
                if not state.get("source"):
                    raise RuntimeError("new workspace/configuration requires --source")
            state.update(owner=owner, last_used=time.time())
            atomic_json(workspace / "state.json", state)
            source, build = paths(workspace, state)
            if owner:
                print(str(source) + "\n" + str(build))
                return
            env = dict(os.environ, OMKTR_WORKSPACE_SOURCE=str(source), OMKTR_WORKSPACE_BUILD=str(build))
            configure = args.configure if args.configure is not None else state.get("configure")
            try:
                if args.configure is not None:
                    state["configure"] = configure
                    if not configure:
                        state.pop("configured", None)
                if configure:
                    signature = hashlib.sha256(encoded([configure, state["source_digest"]])).hexdigest()
                    if state.get("configured") != signature:
                        # A failed replacement may have modified configuration
                        # outputs. Never bless the older successful signature,
                        # or forget the command that must be retried next time.
                        state.pop("configured", None)
                        run_child(["/bin/sh", "-c", configure], build, env)
                        state.update(configure=configure, configured=signature)
                tool = os.environ.get("OMKTR_WORKSPACE_TOOL") or shutil.which("omktr")
                if not command and not tool:
                    raise RuntimeError("cannot locate omktr; supply an explicit command after --")
                run_child(command or [tool, "build"], build, env)
            finally:
                state["last_used"] = time.time()
                atomic_json(workspace / "state.json", state)
            return
        workspace, state = current_profile(base, index)
        if args.action != "status":
            check_owner(state, owner)
        if args.action == "pack":
            print(json.dumps(pack(workspace, state, args.compression)))
        elif args.action in ("unpack", "portage-enter"):
            if owner and state.get("owner") != owner:
                raise WorkspaceBusy("workspace is leased to a different Portage build")
            if owner:
                require_synchronized(state)
            unpack(workspace, state)
            if owner:
                state["last_used"] = time.time()
                atomic_json(workspace / "state.json", state)
                source, build = paths(workspace, state)
                makefile = build / "Makefile"
                if os.environ.get("EBUILD_PHASE") == "compile" and makefile.exists():
                    for line in makefile.read_text(errors="replace").splitlines():
                        if line.startswith("AMDEP_TRUE") and "=" in line and line.split("=", 1)[1].strip().startswith("#"):
                            raise RuntimeError("persistent Automake builds require --enable-dependency-tracking")
                print(str(source) + "\n" + str(build))
        elif args.action == "path":
            unpack(workspace, state)
            print(paths(workspace, state)[1])
        else:
            print(json.dumps(dict(name=args.name, profile=str(workspace), packed=state.get("packed", False),
                                  archive_format=state.get("archive_format", "xz") if state.get("packed") else None,
                                  active_owner=bool(state.get("owner") and owner_busy(state["owner"])),
                                  synchronization_pending=state.get("sync_pending"),
                                  source=state.get("source"), input_count=state.get("input_count", 0),
                                  last_used=state["last_used"])))


if __name__ == "__main__":
    try:
        main()
    except WorkspaceCommandError as error:
        print(f"omktr workspace: {error}", file=sys.stderr)
        raise SystemExit(error.status)
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError) as error:
        print(f"omktr workspace: {error}", file=sys.stderr)
        raise SystemExit(1)
