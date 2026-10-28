#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Manage optional Openmktr Portage configuration. Installation activates nothing."""
import argparse
import difflib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

BEGIN = '# BEGIN openmktr-manager\n'
END = '# END openmktr-manager\n'
PREFIX = 'openmktr-manager-'
OPTIONS = {
    'disabled_graph_optimizations': ('', 'comma-separated features', 'Exclude evaluation, includes, recursive, compact, monitor; exclusions override the profile. Enter none in the TUI to clear all exclusions.'),
    'build_stats': (False, 'boolean', 'Print build timing and optimization statistics; may increase log output.'),
    'regen_debug': (False, 'boolean', 'Explain Ninja regeneration decisions; useful for diagnosing repeated builds.'),
    'mode': ('direct', 'direct|ninja', 'Build mode; Ninja applies to compilation only.'),
    'jobs': ('', 'positive integer', 'Empty inherits Portage MAKEOPTS at build time (unresolved here). Jobs control build recipes, not browsing; use cpus in the TUI for available CPUs.'),
    'load': ('', 'nonnegative number', 'Empty inherits the load limit; zero serializes.'),
    'graph': ('safe', 'off|safe|aggressive', 'Graph optimization profile.'),
    'recursive_cache': (False, 'boolean', 'Conservative recursive no-op certificates.'),
    'strict': (False, 'boolean', 'Reject known incompatible Make constructs.'),
    'workspace': (False, 'boolean', 'Retain prepared sources and outputs; requires Ninja and hooks.'),
    'root': ('/var/cache/openmktr/workspaces', 'absolute path', 'Cache owned by the Portage build user.'),
}


def settings(values):
    if set(values) - set(OPTIONS):
        raise ValueError('unknown setting')
    result = {k: v[0] for k, v in OPTIONS.items()}
    result.update(values)
    excluded = result['disabled_graph_optimizations']
    if not isinstance(excluded, str) or (excluded and any(not x for x in excluded.split(','))) or any(x not in ('evaluation', 'includes', 'recursive', 'compact', 'monitor') for x in excluded.split(',') if x):
        raise ValueError('invalid disabled_graph_optimizations')
    result['disabled_graph_optimizations'] = ','.join(sorted(set(filter(None, excluded.split(',')))))
    if result['mode'] not in ('direct', 'ninja') or result['graph'] not in ('off', 'safe', 'aggressive'):
        raise ValueError('invalid mode or graph profile')
    for key in ('recursive_cache', 'strict', 'workspace', 'build_stats', 'regen_debug'):
        if type(result[key]) is not bool:
            raise ValueError(key + ' must be boolean')
    for key in ('jobs', 'load', 'root', 'mode', 'graph'):
        if not isinstance(result[key], str):
            raise ValueError(key + ' must be text')
    if result['jobs'] and not re.fullmatch(r'[1-9][0-9]*', str(result['jobs'])):
        raise ValueError('jobs must be a positive integer')
    if result['load'] and (not re.fullmatch(r'[0-9]+(?:\.[0-9]+)?', result['load']) or not math.isfinite(float(result['load'])) or float(result['load']) < 0):
        raise ValueError('load must be finite and nonnegative')
    root = str(result['root'])
    if not root.startswith('/var/cache/openmktr/') or '..' in Path(root).parts or any(c in root for c in '\n\r\x00'):
        raise ValueError('workspace root must be below /var/cache/openmktr')
    if result['workspace'] and result['mode'] != 'ninja':
        raise ValueError('persistent workspaces require Ninja mode')
    return result


def option_value(name, value):
    if type(OPTIONS[name][0]) is bool:
        if value.lower() not in ('true', 'false'):
            raise ValueError(name + ' must be true or false')
        return value.lower() == 'true'
    return '' if (name in ('jobs', 'load') and value == 'inherit') or (name == 'disabled_graph_optimizations' and value == 'none') else value


def environment(values, legacy=False):
    v = settings(values)
    lines = [f'export MAKE="/usr/bin/omktr-portage"', f'export OMKTR_PORTAGE_MODE="{v["mode"]}"',
             f'export OMKTR_PORTAGE_GRAPH_OPTIMIZATIONS="{v["graph"]}"',
             f'export OMKTR_PORTAGE_RECURSIVE_CACHE="{int(v["recursive_cache"])}"',
             f'export OMKTR_PORTAGE_STRICT_COMPAT="{int(v["strict"])}"']
    flags = (' -j' + str(v['jobs']) if v['jobs'] else '') + (' -l' + str(v['load']) if v['load'] else '')
    if flags:
        lines.append('MAKEOPTS="${MAKEOPTS}' + flags + '"')
    if v['workspace']:
        lines += ['export OMKTR_WORKSPACE_ROOT=' + shlex.quote(v['root']),
                  'export EXTRA_ECONF="${EXTRA_ECONF} --enable-dependency-tracking"']
    if not legacy:
        lines += [f'export OMKTR_PORTAGE_DISABLED_GRAPH_OPTIMIZATIONS="{v["disabled_graph_optimizations"]}"', f'export OMKTR_PORTAGE_BUILD_STATS="{int(v["build_stats"])}"', f'export OMKTR_PORTAGE_REGEN_DEBUG="{int(v["regen_debug"])}"']
    else:
        for key in ('disabled_graph_optimizations', 'build_stats', 'regen_debug'): v.pop(key)
    return '# SPDX-License-Identifier: MPL-2.0\n# settings: ' + json.dumps(v, sort_keys=True) + '\n' + '\n'.join(lines) + '\n'


class Portage:
    def __init__(self):
        try:
            import portage
        except ImportError as error:
            raise RuntimeError('Portage Python module is required; run with the Gentoo system Python') from error
        self.module = portage
        self.db = portage.db[portage.root]

    def atom(self, value):
        atom = self.module.dep.Atom(value, allow_repo=True)
        if atom.blocker or atom.use:
            raise ValueError('blockers and USE dependencies are not package selections')
        return atom

    def matches(self, atom, package):
        return any(self.db[t].dbapi.match(atom) for t in ('vartree', 'porttree')) if str(atom) == package else bool(
            self.module.match_from_list(atom, self.db['vartree'].dbapi.match(package) + self.db['porttree'].dbapi.match(package)))

    def packages(self):
        installed = set(self.db['vartree'].dbapi.cp_all())
        available = set(self.db['porttree'].dbapi.cp_all())
        return [dict(package=p, installed=p in installed, available=p in available) for p in sorted(installed | available)]


class Backend:
    def __init__(self, root=Path('/etc/portage'), portage=None):
        self.root = Path(root)
        self._portage = portage

    @property
    def portage(self):
        if self._portage is None: self._portage = Portage()
        return self._portage

    def snapshot(self):
        view = Backend(self.root, self.portage)
        cache = {}
        def read(relative):
            key = str(relative)
            if key not in cache: cache[key] = self.read(relative)
            return cache[key]
        view.read = read
        entries = self.entries()
        for name in {name for entry in entries for name in entry[3]}:
            read(Path('env') / name)
        view.entries = lambda: entries
        indexed, broad = {}, []
        for entry in entries:
            atom = self.portage.atom(entry[2])
            key = getattr(atom, 'cp', None)
            if key is None and re.fullmatch(r'[A-Za-z0-9_+.-]+/[A-Za-z0-9_+.-]+', entry[2]): key = entry[2]
            if key and '*' not in key: indexed.setdefault(key, []).append(entry)
            else: broad.append(entry)
        view.entries_for = lambda package: [e for e in indexed.get(package, []) + broad if e[2] == package or self.portage.matches(e[2], package)]
        return view

    def read(self, relative):
        relative = Path(relative)
        if relative.is_absolute() or '..' in relative.parts:
            raise RuntimeError('unsafe configuration path')
        path = self.root / relative
        for item in (path, *path.parents):
            if item.is_symlink():
                raise RuntimeError('symlink configuration requires manual management: ' + str(item))
        return path.read_text() if path.exists() else ''

    def files(self):
        path = self.root / 'package.env'
        return sorted(p.relative_to(self.root) for p in path.rglob('*') if p.is_file()) if path.is_dir() else [Path('package.env')]

    def entries(self):
        result = []
        for path in self.files():
            for number, line in enumerate(self.read(path).splitlines()):
                words = line.split()
                if words and not words[0].startswith('#'):
                    result.append((path, number, words[0], words[1:]))
        return result

    def entries_for(self, package):
        return [e for e in self.entries() if e[2] == package or self.portage.matches(e[2], package)]

    def show(self, package):
        self.portage.atom(package)
        entries = self.entries_for(package)
        managed = [e for e in entries if any(x.startswith(PREFIX) for x in e[3])]
        activations = [(e, name) for e in entries for name in e[3]
                       if self.activates(name)]
        state = 'manual' if activations else 'disabled'
        values = None
        if managed:
            state = 'managed' if len(activations) == 1 and activations[0][0][2] == package else 'conflicting'
            names = [x for x in managed[0][3] if x.startswith(PREFIX)]
            if len(names) == 1:
                text = self.read(Path('env') / names[0])
                try:
                    values = settings(json.loads(text.split('# settings: ', 1)[1].splitlines()[0]))
                    if text not in (environment(values), environment(values, legacy=True)):
                        state = 'indeterminate'
                except (ValueError, IndexError):
                    state = 'indeterminate'
        for entry in entries:
            for name in entry[3]:
                if name.startswith(PREFIX) or name in ('openmktr-direct', 'openmktr-ninja'): continue
                text = self.read(Path('env') / name)
                if not text or re.search(r'\b(?:MAKE|MAKEOPTS|OMKTR_[A-Z_]+|EXTRA_ECONF)\s*=', text) or any(x in text for x in ('$(', '`', 'source ', '. ', 'eval ')) or any(not re.match(r'\s*(?:#|$|(?:export\s+)?[A-Za-z_][A-Za-z_0-9]*=)', line) for line in text.splitlines()):
                    state = 'indeterminate'
        return dict(package=package, state=state, settings=values, entries=[dict(file=str(e[0]), line=e[1]+1, atom=e[2], environments=e[3]) for e in entries])

    def activates(self, name):
        return name.startswith('openmktr-') or '/omktr-portage' in self.read(Path('env') / name)

    def adoption_settings(self, package, entries):
        modes = set()
        for entry in entries:
            for name in entry['environments']:
                if not self.activates(name):
                    continue
                if entry['atom'] != package:
                    raise RuntimeError('overlapping activation atoms require manual resolution')
                if name not in ('openmktr-direct', 'openmktr-ninja'):
                    raise RuntimeError('custom environment cannot be adopted: ' + name)
                mode = name.split('-')[-1]
                expected = f'export MAKE="/usr/bin/omktr-portage"\nexport OMKTR_PORTAGE_MODE="{mode}"\n'
                text = self.read(Path('env') / name)
                clean = '\n'.join(x for x in text.splitlines() if x.strip() and not x.lstrip().startswith('#')) + '\n'
                if clean != expected:
                    raise RuntimeError('custom environment cannot be adopted: ' + name)
                modes.add(mode)
        if len(modes) > 1:
            raise RuntimeError('conflicting activation modes require manual resolution')
        return {'mode': modes.pop()} if modes else {}

    def effective_settings(self, package):
        info = self.show(package)
        if info['state'] in ('conflicting', 'indeterminate'):
            raise RuntimeError(package + ': resolve conflicting/custom entries manually')
        if info['settings'] is not None:
            return info['settings']
        return settings(self.adoption_settings(package, info['entries'])
                        if info['state'] == 'manual' else {})

    def plan(self, action, packages, values):
        updates = {}
        def content(path):
            return updates.get(str(path), self.read(path))
        def replace(path, value):
            updates[str(path)] = value
        for package in sorted(set(packages)):
            self.portage.atom(package)
            info = self.show(package)
            if info['state'] in ('conflicting', 'indeterminate'):
                raise RuntimeError(package + ': resolve conflicting/custom entries manually')
            if info['state'] == 'manual' and action != 'adopt':
                raise RuntimeError(package + ': explicit adopt is required')
            adopted_values = self.adoption_settings(package, info['entries']) if action == 'adopt' else {}
            for path in self.files():
                lines = content(path).splitlines(keepends=True)
                for i, line in enumerate(lines):
                    words = line.split()
                    if words and words[0] == package:
                        keep = [x for x in words[1:] if not x.startswith(PREFIX) and not (action == 'adopt' and x in ('openmktr-direct', 'openmktr-ninja'))]
                        lines[i] = package + ' ' + ' '.join(keep) + '\n' if keep else ''
                replace(path, ''.join(lines))
            if action != 'disable':
                v = settings({**adopted_values, **(info['settings'] or {}), **values})
                if v['workspace'] and BEGIN not in self.read('bashrc'):
                    raise RuntimeError('run setup-workspaces before enabling persistent workspaces')
                name = PREFIX + hashlib.sha256(package.encode()).hexdigest()[:24]
                previous = self.read(Path('env') / name)
                if previous:
                    try:
                        old = settings(json.loads(previous.split('# settings: ', 1)[1].splitlines()[0]))
                        if previous not in (environment(old), environment(old, legacy=True)): raise ValueError('custom environment')
                    except (ValueError, IndexError) as error:
                        raise RuntimeError('reserved environment file contains custom settings') from error
                replace(Path('env') / name, environment(v))
                path = Path('package.env/openmktr-manager') if (self.root / 'package.env').is_dir() else Path('package.env')
                text = content(path)
                if BEGIN not in text:
                    text += ('\n' if text and not text.endswith('\n') else '') + BEGIN + END
                if text.count(BEGIN) != 1 or text.count(END) != 1:
                    raise RuntimeError('invalid manager block')
                text = text.replace(END, package + ' ' + name + '\n' + END)
                head, block = text.split(BEGIN)
                body, tail = block.split(END)
                replace(path, head + BEGIN + ''.join(sorted(body.splitlines(True))) + END + tail)
        changes = {p: dict(before=self.read(p), after=v) for p, v in updates.items() if self.read(p) != v}
        return dict(action=action, packages=packages, values=values, changes=changes)

    def setup(self, values=None):
        values = {'root': settings(values or {})['root']}
        text = self.read('bashrc')
        block = BEGIN + 'if [[ -n ${OMKTR_WORKSPACE_ROOT} ]]; then\n source /usr/share/openmktr/workspace-hooks.sh\nfi\n' + END
        if BEGIN in text:
            if block not in text:
                raise RuntimeError('custom workspace hooks require manual resolution')
            after = text
        elif text.strip():
            raise RuntimeError('existing bashrc requires manual hook composition')
        else:
            after = text + block
        return dict(action='setup-workspaces', packages=[], values=values, changes={'bashrc': dict(before=text, after=after)} if text != after else {})

    def recover(self):
        import fcntl
        with (self.root / '.openmktr-manager.lock').open('a') as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            journal = self.root / '.openmktr-manager-transaction'
            plan = json.loads(self.read(journal.name))
            for relative, change in plan['changes'].items():
                if self.read(relative) not in (change['before'], change['after']):
                    raise RuntimeError('manual edits prevent recovery: ' + relative)
            for relative, change in reversed(list(plan['changes'].items())):
                atomic(self.root / relative, change['before'])
            journal.unlink()

    def apply(self, plan):
        import fcntl
        self.root.mkdir(parents=True, exist_ok=True)
        lock = self.root / '.openmktr-manager.lock'
        self.read(lock.name)
        with lock.open('a') as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            expected = self.setup(plan['values']) if plan['action'] == 'setup-workspaces' else self.plan(plan['action'], plan['packages'], plan['values'])
            if expected != plan:
                raise RuntimeError('configuration changed since review; refresh the preview')
            journal = self.root / '.openmktr-manager-transaction'
            if journal.exists():
                raise RuntimeError('interrupted transaction: inspect .openmktr-manager-transaction before retrying')
            if plan['action'] == 'setup-workspaces':
                import pwd
                owner = pwd.getpwnam('portage')
                cache = Path(plan['values']['root'])
                if any(x.is_symlink() for x in (cache, *cache.parents)):
                    raise RuntimeError('cache must not be a symlink')
                if cache.exists() and cache.stat().st_uid != owner.pw_uid:
                    raise RuntimeError('existing cache must belong to the Portage build user')
                cache.mkdir(parents=True, exist_ok=True)
                os.chown(cache, owner.pw_uid, owner.pw_gid)
                cache.chmod(0o700)
            atomic(journal, json.dumps(plan))
            try:
                for relative, change in sorted(plan['changes'].items(), key=lambda x: not x[0].startswith('env/')):
                    path = self.root / relative
                    self.read(relative)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    atomic(path, change['after'])
                journal.unlink()
            except BaseException:
                # Journal retains exact before/after bytes for explicit recovery.
                raise


def atomic(path, text):
    fd, name = tempfile.mkstemp(dir=path.parent, prefix='.openmktr-')
    try:
        mode = path.stat().st_mode & 0o777 if path.exists() else 0o644
        with os.fdopen(fd, 'w') as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def diff(plan):
    return ''.join(''.join(difflib.unified_diff(c['before'].splitlines(True), c['after'].splitlines(True), fromfile=p, tofile=p)) for p, c in plan['changes'].items()) or 'No changes.\n'


def save(backend, plan):
    if os.geteuid() == 0:
        backend.apply(plan)
        return
    helper = Path('/usr/bin/omktr-manager')
    if not helper.exists():
        raise RuntimeError('install the manager before elevation, or run the CLI with administrative privileges')
    result = subprocess.run(['pkexec', str(helper), '_apply'], input=json.dumps(plan), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr or 'Authentication cancelled or unavailable; pending edits retained. Use the privileged CLI to apply.')


class Controller:
    def __init__(self, backend):
        self.backend = backend
        self.pending = None
        self.reviewed = False

    def initial_settings(self, packages):
        if not packages:
            raise RuntimeError('select at least one package')
        values = [self.backend.effective_settings(p) for p in sorted(set(packages))]
        return {key: values[0][key] if all(v[key] == values[0][key] for v in values) else None
                for key in OPTIONS}

    def stage(self, action, packages, values):
        if self.pending:
            raise RuntimeError('Save or discard the pending batch before staging another batch')
        self.reviewed = False
        self.pending = self.backend.setup(values) if action == 'setup-workspaces' else self.backend.plan(action, packages, values)
        return diff(self.pending)

    def save(self):
        if self.pending:
            save(self.backend, self.pending)
            self.pending = None


def workspace(request, elevate=True):
    allowed = {'action', 'root', 'name', 'profile', 'compression', 'idle_seconds'}
    if set(request) - allowed:
        raise ValueError('unknown workspace request field')
    action = request.get('action', 'list')
    if action not in ('list', 'status', 'pack', 'unpack', 'gc'):
        raise ValueError('invalid workspace action')
    root = settings({'root': request.get('root', OPTIONS['root'][0])})['root']
    for parent in (Path(root), *Path(root).parents):
        if parent.is_symlink():
            raise ValueError('workspace root must not traverse symlinks')
    if elevate and os.geteuid() != 0:
        result = subprocess.run(['pkexec', '/usr/bin/omktr-manager', '_workspace'],
                                input=json.dumps(request), text=True, capture_output=True)
    else:
        command = ['/usr/bin/omktr', 'workspace', action, '--root', root]
        for key in ('name', 'profile'):
            value = request.get(key)
            if value:
                if not isinstance(value, str) or '\n' in value or '\x00' in value:
                    raise ValueError('invalid workspace selector')
                command += ['--' + key, value]
        if action in ('pack', 'gc'):
            compression = request.get('compression', 'xz')
            if compression not in ('xz', 'zstd'): raise ValueError('invalid compression')
            command += ['--compression', compression]
        if action == 'gc':
            idle = float(request.get('idle_seconds', 3600))
            if not math.isfinite(idle) or idle < 0: raise ValueError('invalid idle age')
            command += ['--idle-seconds', str(idle)]
        result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr or 'Workspace operation cancelled or failed')
    return result.stdout


class SettingsDraft:
    """Keep untouched values independent for every package in a batch."""
    def __init__(self, initial):
        self.initial = initial
        self.edits = {}

    def set(self, key, value):
        if value == self.initial[key]:
            self.edits.pop(key, None)
        else:
            self.edits[key] = value


def gui(backend):
    try:
        import tkinter as tk
        from tkinter import ttk, messagebox
    except ImportError as error:
        raise RuntimeError('install Python Tkinter / enable the gui USE flag') from error
    import threading
    import queue
    app = tk.Tk()
    app.title('Openmktr Portage Manager')
    app.geometry(f'{min(1280, app.winfo_screenwidth()-60)}x{min(850, app.winfo_screenheight()-80)}')
    app.minsize(640, 480)
    normal_geometry = []
    def fullscreen(event=None):
        enabled = bool(app.attributes('-fullscreen'))
        if not enabled: normal_geometry[:] = [app.geometry()]
        app.attributes('-fullscreen', not enabled)
        if enabled and normal_geometry: app.geometry(normal_geometry[0])
    app.bind('<F11>', fullscreen)
    app.bind('<Escape>', lambda event: fullscreen() if app.attributes('-fullscreen') else None)
    from tkinter import font as tkfont
    if 'Liberation Sans' in tkfont.families(app):
        for name in ('TkDefaultFont', 'TkTextFont', 'TkMenuFont', 'TkHeadingFont'):
            tkfont.nametofont(name).configure(family='Liberation Sans', size=10)
        tkfont.nametofont('TkFixedFont').configure(family='Liberation Mono', size=9)
    family = tkfont.nametofont('TkDefaultFont').actual('family')
    style = ttk.Style(app)
    if 'clam' in style.theme_names(): style.theme_use('clam')
    style.configure('.', background='#f3f5f7', foreground='#243447')
    style.configure('TNotebook', background='#f3f5f7', borderwidth=0)
    style.configure('Treeview', background='white', fieldbackground='white', rowheight=27)
    style.map('Treeview', background=[('selected', '#d9eafa')], foreground=[('selected', '#173b61')])
    style.map('TNotebook.Tab', background=[('selected', 'white')])
    style.configure('TButton', padding=(10, 6))
    style.configure('TNotebook.Tab', padding=(14, 8))
    style.configure('Heading.TLabel', font=(family, 17, 'bold'))
    style.configure('Title.TLabel', font=(family, 11, 'bold'))
    controller = Controller(backend)
    selected, rows, information = [], [], {}
    draft = SettingsDraft({k: v[0] for k, v in OPTIONS.items()})
    loading = False
    rendering = False
    busy = False
    events = queue.Queue()
    poll_timer = []
    auxiliary_buttons = []
    cpu_button = None
    def auxiliary(parent, **options):
        button = ttk.Button(parent, **options)
        auxiliary_buttons.append(button)
        return button
    shell = ttk.Frame(app, padding=18)
    shell.pack(fill='both', expand=True)
    ttk.Label(shell, text='Openmktr', style='Heading.TLabel').pack(anchor='w')
    ttk.Label(shell, text='Portage Manager  /  Select packages, adjust settings, review and save.').pack(anchor='w', pady=(4, 16))
    ttk.Button(shell, text='Fullscreen (F11)', command=fullscreen).pack(anchor='e')
    navigation = ttk.Notebook(shell)
    navigation.pack(fill='both', expand=True)
    package_page = ttk.Frame(navigation, padding=14)
    workspace_page = ttk.Frame(navigation, padding=18)
    navigation.add(package_page, text='Packages')
    navigation.add(workspace_page, text='Workspace manager')
    split = ttk.Panedwindow(package_page, orient='horizontal')
    split.pack(fill='both', expand=True)
    browser = ttk.Frame(split, padding=(0, 0, 16, 0))
    editor = ttk.Frame(split, padding=(16, 0, 0, 0))
    split.add(browser, weight=2)
    split.add(editor, weight=3)
    ttk.Label(browser, text='Package catalog', style='Title.TLabel').pack(anchor='w')
    query = tk.StringVar(app)
    scope = tk.StringVar(app, value='all')
    ttk.Label(browser, text='Search category or package').pack(anchor='w', pady=(12, 4))
    search = ttk.Entry(browser, textvariable=query)
    search.pack(fill='x')
    ttk.Combobox(browser, textvariable=scope, values=('all', 'installed', 'available', 'configured'), state='readonly').pack(fill='x', pady=8)
    holder = ttk.Frame(browser)
    holder.pack(fill='both', expand=True)
    table = ttk.Treeview(holder, columns=('installed', 'state'), selectmode='extended')
    for key, label, width in (('#0', 'Package', 205), ('installed', 'Installed', 65), ('state', 'Configuration', 100)):
        table.heading(key, text=label)
        table.column(key, width=width, minwidth=40, stretch=key == '#0')
    scrollbar = ttk.Scrollbar(holder, command=table.yview)
    table.configure(yscrollcommand=scrollbar.set)
    table.pack(side='left', fill='both', expand=True)
    scrollbar.pack(side='right', fill='y')
    count = tk.StringVar(app, value='Loading catalog...')
    ttk.Label(browser, textvariable=count).pack(anchor='w', pady=8)
    title = tk.StringVar(app, value='Select a package')
    hint = tk.StringVar(app, value='Use Ctrl or Shift to select multiple packages.')
    ttk.Label(editor, textvariable=title, style='Title.TLabel').pack(anchor='w')
    hint_label = ttk.Label(editor, textvariable=hint, wraplength=510)
    hint_label.pack(anchor='w', pady=(8, 12))
    editor.bind('<Configure>', lambda e: hint_label.configure(wraplength=max(80, e.width-32)))
    tabs = ttk.Notebook(editor)
    tabs.pack(fill='both', expand=True)
    variables, fields, inherits, pages = {}, {}, {}, {}
    inherit_fields = {}
    reset_buttons = []
    groups = [('Build', ('mode', 'strict')), ('Performance', ('jobs', 'load', 'graph', 'recursive_cache', 'disabled_graph_optimizations', 'build_stats', 'regen_debug')), ('Workspaces', ('workspace', 'root'))]
    labels = dict(disabled_graph_optimizations='Excluded graph optimizations', build_stats='Build statistics', regen_debug='Regeneration explanations', mode='Build mode', strict='Strict compatibility', jobs='Parallel jobs', load='Load limit', graph='Graph optimizations', recursive_cache='Recursive no-op cache', workspace='Persistent workspace', root='Workspace cache root')
    error = tk.StringVar(app)
    status = tk.StringVar(app, value='Loading package catalog...')

    def changed(key):
        if loading or busy: return
        value = variables[key].get()
        if type(OPTIONS[key][0]) is bool:
            value = None if value == -1 else bool(value)
            fields[key].configure(text='Mixed (unchanged)' if value is None else 'Enabled')
        if key in inherits and inherits[key].get(): value = ''
        if value is None or value == 'Mixed (unchanged)': draft.edits.pop(key, None)
        else: draft.set(key, value)
        if key in inherits:
            fields[key].configure(state='disabled' if inherits[key].get() else 'normal')
        error.set('')

    def reset(key):
        nonlocal loading
        if busy: return
        if not loading and controller.pending:
            controller.pending = None
            write_preview('')
        loading = True
        value = draft.initial[key]
        variables[key].set((-1 if type(OPTIONS[key][0]) is bool else 'Mixed (unchanged)') if value is None else value)
        if key in inherits: inherits[key].set(value == '')
        draft.edits.pop(key, None)
        if type(OPTIONS[key][0]) is bool:
            fields[key].state(['alternate'] if value is None else ['!alternate'])
            fields[key].configure(text='Mixed (unchanged)' if value is None else 'Enabled')
        loading = False
        controls()

    for name, keys in groups:
        container = ttk.Frame(tabs)
        canvas = tk.Canvas(container, width=1, highlightthickness=0)
        bar = ttk.Scrollbar(container, command=canvas.yview)
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y'); canvas.pack(fill='both', expand=True)
        page = ttk.Frame(canvas, padding=16)
        window = canvas.create_window(0, 0, anchor='nw', window=page)
        page.bind('<Configure>', lambda e, c=canvas: c.configure(scrollregion=c.bbox('all')))
        def resize_settings(event, c=canvas, w=window, page=page):
            c.itemconfigure(w, width=event.width)
            for child in page.winfo_children():
                if isinstance(child, ttk.Label): child.configure(wraplength=max(80, event.width-40))
        canvas.bind('<Configure>', resize_settings)
        page.columnconfigure(0, weight=1)
        tabs.add(container, text=name)
        page.tab_container = container
        pages[name] = page
        for index, key in enumerate(keys):
            row = index * 4
            default, kind, help_text = OPTIONS[key]
            ttk.Label(page, text=labels[key], style='Title.TLabel').grid(row=row, column=0, sticky='w', pady=(8, 4))
            variable = tk.IntVar(app, value=-1) if type(default) is bool else tk.StringVar(app)
            variables[key] = variable
            if type(default) is bool:
                field = ttk.Checkbutton(page, text='Enabled', variable=variable, command=lambda k=key: changed(k))
            elif key in ('mode', 'graph'):
                field = ttk.Combobox(page, textvariable=variable, width=12, values=kind.split('|') + ['Mixed (unchanged)'], state='readonly')
                field.bind('<<ComboboxSelected>>', lambda e, k=key: changed(k))
            else:
                field = ttk.Entry(page, textvariable=variable, width=12)
                variable.trace_add('write', lambda *args, k=key: changed(k))
            fields[key] = field
            field.grid(row=row+1, column=0, sticky='ew')
            reset_button = ttk.Button(page, text='Keep existing', command=lambda k=key: reset(k))
            reset_button.grid(row=row+1, column=1, padx=(8, 0))
            reset_buttons.append(reset_button)
            ttk.Label(page, text=help_text + ' Default: ' + (str(default) if type(default) is bool else str(default or 'inherit / none')) + '.', wraplength=320).grid(row=row+2, column=0, columnspan=2, sticky='w', pady=(4, 6))
            if key in ('jobs', 'load'):
                inherits[key] = tk.BooleanVar(app)
                inherit_fields[key] = ttk.Checkbutton(page, text='Inherit from Portage', variable=inherits[key], command=lambda k=key: changed(k))
                inherit_fields[key].grid(row=row+3, column=0, sticky='w')
    review = ttk.Frame(tabs, padding=12)
    tabs.add(review, text='Review')
    summary = tk.StringVar(app, value='Review changes to see the affected packages and exact file diff.')
    ttk.Label(review, textvariable=summary, wraplength=510).pack(anchor='w', pady=(0, 8))
    diff_frame = ttk.Frame(review)
    diff_frame.pack(fill='both', expand=True)
    preview = tk.Text(diff_frame, wrap='none', height=14, width=45, font='TkFixedFont', state='disabled')
    xbar = ttk.Scrollbar(diff_frame, orient='horizontal', command=preview.xview)
    ybar = ttk.Scrollbar(diff_frame, command=preview.yview)
    preview.configure(xscrollcommand=xbar.set, yscrollcommand=ybar.set)
    xbar.pack(side='bottom', fill='x'); ybar.pack(side='right', fill='y')
    preview.pack(fill='both', expand=True)
    ttk.Label(editor, textvariable=error, foreground='#a32b26', wraplength=510).pack(anchor='w', pady=8)
    actions = ttk.Frame(editor); actions.pack(fill='x')
    ttk.Label(shell, textvariable=status).pack(anchor='w', pady=(12, 0))

    def write_preview(text):
        preview.configure(state='normal'); preview.delete('1.0', 'end'); preview.insert('1.0', text); preview.configure(state='disabled')

    def permit_discard():
        return not (draft.edits or controller.pending) or messagebox.askyesno('Discard changes?', 'Discard current unsaved changes?', parent=app)

    def controls():
        for button in auxiliary_buttons:
            button.configure(state='disabled' if busy else 'normal')
        for button in operation_buttons: button.configure(state='disabled' if busy else 'normal')
        states = [information[p]['state'] for p in selected]
        valid = bool(selected) and not any(s in ('conflicting', 'indeterminate') for s in states)
        if cpu_button is not None: cpu_button.configure(state='normal' if valid and not busy and not controller.pending else 'disabled')
        for widget, enabled in ((configure, valid and 'manual' not in states), (adopt, valid and 'manual' in states), (disable, valid and 'manual' not in states), (save_button, bool(controller.pending))):
            widget.configure(state='normal' if enabled and not busy and (widget == save_button or not controller.pending) else 'disabled')
        for widget in [*inherit_fields.values(), *reset_buttons]:
            widget.configure(state='normal' if valid and not busy and not controller.pending else 'disabled')
        for key, field in fields.items():
            inherited = key in inherits and inherits[key].get()
            field.configure(state='disabled' if not valid or busy or controller.pending or inherited else 'readonly' if isinstance(field, ttk.Combobox) else 'normal')

    def clear():
        if busy: return
        controller.pending = None
        for key in variables: reset(key)
        write_preview(''); error.set(''); controls()

    def initial_from_information(packages):
        for p in packages:
            if information[p].get('effective_error'): raise RuntimeError(p + ': ' + information[p]['effective_error'])
        if any(information[p]['state'] in ('conflicting', 'indeterminate') for p in packages):
            raise RuntimeError('Resolve conflicting or custom package environments manually before editing.')
        values = [information[p].get('effective') or settings({}) for p in packages]
        return {k: values[0][k] if all(v[k] == values[0][k] for v in values) else None for k in OPTIONS}

    def selection(event=None):
        nonlocal selected, draft
        if loading or rendering: return
        if busy:
            table.selection_set([p for p in selected if table.exists(p)])
            return
        upcoming = [p for p in selected if not table.exists(p)] + list(table.selection())
        if set(upcoming) == set(selected): return
        if not permit_discard():
            table.selection_set([p for p in selected if table.exists(p)]); return
        selected = upcoming
        controller.pending = None
        write_preview('')
        try:
            draft = SettingsDraft(initial_from_information(selected) if selected else {k: v[0] for k, v in OPTIONS.items()})
            hint.set('Mixed values stay unchanged until edited.' if len(selected) > 1 else 'Edit settings below, then review changes before saving.')
        except (RuntimeError, ValueError, OSError) as exc:
            draft = SettingsDraft({k: v[0] for k, v in OPTIONS.items()})
            hint.set(str(exc))
        for key in variables: reset(key)
        title.set(selected[0] if len(selected) == 1 else str(len(selected)) + ' packages selected')
        count.set(str(len(table.get_children())) + ' results / ' + str(len(selected)) + ' selected')
        error.set(''); controls()

    render_generation = [0]
    search_timer = []
    def refresh(*args):
        nonlocal rendering
        if busy: return
        render_generation[0] += 1
        generation = render_generation[0]
        rendering = True
        table.delete(*table.get_children())
        filtered = [r for r in rows if query.get().lower() in r['package'].lower() and
                    (scope.get() == 'all' or (scope.get() == 'configured' and information[r['package']]['state'] != 'disabled') or r.get(scope.get(), False))]
        def batch(offset=0):
            nonlocal rendering
            if generation != render_generation[0]: return
            for row in filtered[offset:offset+100]:
                p = row['package']
                table.insert('', 'end', iid=p, text=p, values=('Yes' if row['installed'] else 'No', information[p]['state']))
            if offset+100 < len(filtered): app.after(1, lambda: batch(offset+100))
            else:
                table.selection_set([p for p in selected if table.exists(p)])
                rendering = False
                count.set(str(len(filtered)) + ' results / ' + str(len(selected)) + ' selected')
        batch()

    def search_changed(*args):
        if search_timer: app.after_cancel(search_timer.pop())
        search_timer[:] = [app.after(200, lambda: (search_timer.clear(), refresh()))]

    def stage(action):
        if busy: return
        try:
            if action in ('configure', 'adopt'):
                for package in selected: settings({**backend.effective_settings(package), **draft.edits})
            values = draft.edits if action in ('configure', 'adopt') else {'root': variables['root'].get()} if action == 'setup-workspaces' else {}
            text = controller.stage(action, selected, values)
            summary.set(action.title() + ': ' + (', '.join(selected) if action != 'setup-workspaces' else 'Portage hooks') + '\n' + ', '.join(labels[k] + ': ' + str(v) for k, v in values.items()))
            write_preview(text); tabs.select(review); controls()
        except (ValueError, RuntimeError, OSError) as exc:
            error.set(str(exc))
            key = next((k for k in fields if k in str(exc)), next(iter(draft.edits), 'mode'))
            tabs.select(pages[next(name for name, keys in groups if key in keys)].tab_container)
            fields[key].focus_set()

    def back():
        if busy: return
        controller.pending = None; write_preview(''); tabs.select(pages['Build'].tab_container); controls()

    def background(work, done, description='Working; controls are temporarily unavailable...'):
        nonlocal busy
        if busy: return
        busy = True; error.set(''); status.set(description); controls()
        def worker():
            try: events.put((done, work(), None))
            except Exception as exc: events.put((done, None, exc))
        threading.Thread(target=worker, daemon=True).start()

    def poll():
        nonlocal busy
        try:
            done, result, exc = events.get_nowait()
            busy = False
            status.set((str(exc).strip() or type(exc).__name__ + ': operation failed') if exc else 'Ready')
            if exc: error.set(str(exc).strip() or type(exc).__name__ + ': operation failed')
            else: done(result)
            controls()
        except queue.Empty: pass
        poll_timer[:] = [app.after(60, poll)]

    def discover():
        catalog = backend.portage.packages()
        snapshot = backend.snapshot()
        information = {}
        for row in catalog:
            p = row['package']; info = snapshot.show(p)
            if info['state'] not in ('conflicting', 'indeterminate'):
                try: info['effective'] = snapshot.effective_settings(p)
                except (ValueError, RuntimeError, OSError) as exc:
                    info['effective_error'] = str(exc).strip() or type(exc).__name__ + ': settings unavailable'
            information[p] = info
        return catalog, information

    def loaded(result):
        nonlocal rows, information, selected
        rows, information = result
        selected = [p for p in selected if p in information]
        refresh()
        if selected:
            try: draft.initial = initial_from_information(selected)
            except (ValueError, RuntimeError, OSError) as exc:
                draft.initial = {k: v[0] for k, v in OPTIONS.items()}
                hint.set(str(exc))
            for key in variables: reset(key)

    def commit():
        if not controller.pending: return
        def work():
            controller.save()
            return discover()
        def done(result):
            draft.edits.clear(); loaded(result); status.set('Changes saved')
        background(work, done)

    configure = ttk.Button(actions, text='Review changes', command=lambda: stage('configure')); configure.pack(side='left')
    adopt = ttk.Button(actions, text='Adopt', command=lambda: stage('adopt')); adopt.pack(side='left', padx=8)
    disable = ttk.Button(actions, text='Disable', command=lambda: stage('disable')); disable.pack(side='left')
    auxiliary(review, text='Back to settings', command=back).pack(side='left', pady=10)
    save_button = ttk.Button(review, text='Save changes', command=commit); save_button.pack(side='left', padx=8)
    auxiliary(review, text='Discard changes', command=lambda: clear() if permit_discard() else None).pack(side='left')
    ttk.Label(pages['Workspaces'], text='Persistent workspaces require Ninja mode and installed Portage hooks.', wraplength=440).grid(row=9, column=0, columnspan=2, sticky='w', pady=16)
    auxiliary(pages['Workspaces'], text='Review workspace setup', command=lambda: stage('setup-workspaces')).grid(row=10, column=0, sticky='w')
    ttk.Label(workspace_page, text='Retained workspaces', style='Title.TLabel').pack(anchor='w')
    ttk.Label(workspace_page, text='Private operations may require authentication. Compress idle retains all data.').pack(anchor='w', pady=12)
    form = ttk.Frame(workspace_page); form.pack(fill='x')
    workspace_vars = {}
    for row, (key, label, default) in enumerate((('root', 'Cache root', OPTIONS['root'][0]), ('name', 'Workspace name (from List)', ''), ('profile', 'Profile (empty selects current)', ''), ('compression', 'Compression', 'xz'), ('idle_seconds', 'Idle age in seconds', '3600'))):
        ttk.Label(form, text=label).grid(row=row, column=0, sticky='w', padx=(0, 16), pady=6)
        workspace_vars[key] = tk.StringVar(app, value=default)
        field = ttk.Combobox(form, textvariable=workspace_vars[key], values=('xz', 'zstd'), state='readonly') if key == 'compression' else ttk.Entry(form, textvariable=workspace_vars[key])
        field.grid(row=row, column=1, sticky='ew')
    form.columnconfigure(1, weight=1)
    toolbar = ttk.Frame(workspace_page); toolbar.pack(fill='x', pady=14)
    output = tk.Text(workspace_page, wrap='word', height=15, state='disabled'); output.pack(fill='both', expand=True)
    operation_buttons = []
    def operation(action):
        keys = ['root']
        if action in ('status', 'pack', 'unpack'): keys += ['name', 'profile']
        if action in ('pack', 'gc'): keys.append('compression')
        if action == 'gc': keys.append('idle_seconds')
        request = {key: workspace_vars[key].get() for key in keys}
        request['action'] = action
        if action in ('status', 'pack', 'unpack') and not request['name']:
            status.set('Enter a workspace name from Refresh / List.'); return
        def done(result):
            output.configure(state='normal'); output.delete('1.0', 'end'); output.insert('1.0', result.strip() or ('No workspaces found.' if action == 'list' else 'Operation completed without output.')); output.configure(state='disabled')
        background(lambda: workspace(request), done)
    for label, action in (('Refresh / List', 'list'), ('Status', 'status'), ('Pack', 'pack'), ('Unpack', 'unpack'), ('Compress idle', 'gc')):
        button = ttk.Button(toolbar, text=label, command=lambda a=action: operation(a))
        button.pack(side='left', padx=(0, 8))
        operation_buttons.append(button)
    def tab_changed(event=None):
        if busy and controller.pending:
            tabs.select(review)
            return
        if controller.pending and str(tabs.select()) != str(review):
            controller.pending = None
            write_preview('')
            controls()
    tabs.bind('<<NotebookTabChanged>>', tab_changed)
    table.bind('<<TreeviewSelect>>', selection)
    query.trace_add('write', search_changed); scope.trace_add('write', refresh)
    search.bind('<Return>', refresh)
    auxiliary(browser, text='Search', command=refresh).pack(fill='x')
    auxiliary(browser, text='Clear', command=lambda: (query.set(''), refresh())).pack(fill='x')
    def reload_catalog():
        if busy: return
        if permit_discard():
            clear(); background(discover, loaded, 'Refreshing package catalog...')
    auxiliary(browser, text='Refresh catalog', command=reload_catalog).pack(fill='x')
    cpu_button = ttk.Button(pages['Performance'], text='Use available CPUs', command=lambda: (inherits['jobs'].set(False), variables['jobs'].set(str(available_cpus()))))
    cpu_button.grid(row=40, column=0, sticky='w')
    app.bind('<Control-f>', lambda event: search.focus_set())
    app.protocol('WM_DELETE_WINDOW', lambda: app.destroy() if not busy and permit_discard() else None)
    app.manager = dict(table=table, variables=variables, inherits=inherits, tabs=tabs, pages=pages, review=review, stage=stage, back=back, clear=clear, commit=commit, controller=controller, query=query, navigation=navigation, workspace_vars=workspace_vars, operation=operation, error=error, hint=hint)
    controls()
    poll_timer[:] = [app.after(60, poll)]
    def destroyed(event):
        if event.widget is app:
            for timer in app.tk.splitlist(app.tk.call('after', 'info')): app.after_cancel(timer)
    app.bind('<Destroy>', destroyed)
    app.after(0, lambda: background(discover, loaded, 'Loading package catalog...'))
    app.mainloop()

def available_cpus():
    try: return max(1, len(os.sched_getaffinity(0)))
    except (AttributeError, OSError): return max(1, (getattr(os, 'process_cpu_count', os.cpu_count)() or 1))


def tui(backend):
    try:
        import curses
    except ImportError as error:
        raise RuntimeError('install Python curses / enable the tui USE flag') from error
    controller = Controller(backend)
    from concurrent.futures import ThreadPoolExecutor
    executor = ThreadPoolExecutor(max_workers=1)
    def run(screen):
        selected, index, query, message, scope = set(), 0, '', '', 'all'
        def work(function):
            future = executor.submit(function)
            while not future.done():
                screen.erase()
                screen.addnstr(0, 0, 'Working... please wait', max(1, screen.getmaxyx()[1]-1))
                screen.refresh()
                import time
                time.sleep(.02)
            return future.result()
        def discover():
            package_rows = backend.portage.packages()
            snapshot = backend.snapshot()
            return package_rows, {r['package']: snapshot.show(r['package']) for r in package_rows}
        package_rows, information = work(discover)
        row_index = {r['package']: r for r in package_rows}
        rows = [r['package'] for r in package_rows]
        def ask(prompt):
            if '\n' in prompt:
                pager(prompt)
                return ''
            screen.clear()
            screen.addstr(0, 0, prompt[:max(1, screen.getmaxyx()[1]-1)])
            curses.echo()
            try:
                return screen.getstr(1, 0).decode()
            finally:
                curses.noecho()
        def pager(text):
            lines, offset = text.splitlines(), 0
            while True:
                screen.erase()
                h, w = screen.getmaxyx()
                for n, line in enumerate(lines[offset:offset+max(1,h-2)]):
                    screen.addnstr(n, 0, line, max(1,w-1))
                screen.addnstr(h-1, 0, 'Up/Down scroll; Enter returns', max(1,w-1))
                key = screen.getch()
                if key in (10, 13, 27): return
                if key == curses.KEY_DOWN: offset = min(max(0,len(lines)-1),offset+1)
                if key == curses.KEY_UP: offset = max(0,offset-1)
        previous_filter, visible = None, []
        while True:
            if previous_filter != (query, scope):
                visible = [p for p in rows if query.lower() in p.lower() and (scope == 'all' or (scope == 'configured' and information[p]['state'] != 'disabled') or row_index[p].get(scope, False))]
                previous_filter = (query, scope)
            index = max(0, min(index, len(visible)-1))
            screen.erase()
            height, width = screen.getmaxyx()
            lines = ['Openmktr: arrows navigate; space selects; / search; f filter; c configure; a adopt; d disable', 'w workspace setup; x workspace maintenance; r review; s save; z discard; q quit; R refresh; ? help']
            for p in visible[max(0, index-height+5):index+height-4]:
                lines.append(('> ' if visible[index] == p else '  ') + ('[x] ' if p in selected else '[ ] ') + p + ' [' + information[p]['state'] + ']')
            lines.append(message)
            for n, line in enumerate(lines[:height-1]):
                screen.addnstr(n, 0, line, max(1, width-1))
            key = screen.getch()
            try:
                if key == curses.KEY_DOWN: index += 1
                elif key == curses.KEY_UP: index -= 1
                elif key == ord(' ') and visible: selected.symmetric_difference_update([visible[index]])
                elif key == ord('/'): query = ask('Search category/package:')
                elif key == ord('f'):
                    scope = ask('Filter: all, installed, available, configured')
                    if scope not in ('all', 'installed', 'available', 'configured'): scope = 'all'
                elif key in (ord('c'), ord('a'), ord('d'), ord('w')):
                    action = {ord('c'):'configure', ord('a'):'adopt', ord('d'):'disable', ord('w'):'setup-workspaces'}[key]
                    values = {}
                    if action in ('configure', 'adopt'):
                        initial = controller.initial_settings(sorted(selected))
                        for name, (default, kind, help_text) in OPTIONS.items():
                            current = initial[name] if initial[name] is not None else '<mixed>'
                            value = ask(f'{name}: {help_text} ({kind}; current {current}; Enter keeps; inherit clears jobs/load; jobs=cpus uses available CPUs)')
                            if value == 'cpus' and name == 'jobs': value = str(available_cpus())
                            if value:
                                values[name] = option_value(name, value)
                    controller.stage(action, sorted(selected), values)
                    message = 'Staged. Press r to review, then s to save.'
                elif key == ord('x'):
                    action = ask('Workspace action: list, status, pack, unpack, gc')
                    request = dict(action=action)
                    if action in ('status', 'pack', 'unpack'):
                        request['name'] = ask('Workspace name:')
                        request['profile'] = ask('Profile (empty = current):')
                    if action in ('pack', 'gc'): request['compression'] = ask('Compression: xz or zstd') or 'xz'
                    if action == 'gc': request['idle_seconds'] = ask('Idle seconds (default 3600):') or 3600
                    pager(work(lambda: workspace(request)) or 'Operation completed without output.')
                elif key == ord('r'):
                    pager(diff(controller.pending) if controller.pending else 'No pending edits.')
                    controller.reviewed = True
                elif key == ord('s'):
                    if not controller.reviewed: raise RuntimeError('Press r to review before saving')
                    work(controller.save)
                    package_rows, information = work(discover)
                    rows = [r['package'] for r in package_rows]
                    row_index = {r['package']: r for r in package_rows}
                    selected.intersection_update(rows)
                    previous_filter = None; message = 'Saved.'
                elif key == ord('R'):
                    if not controller.pending or ask('Discard pending edits and refresh? type yes:') == 'yes':
                        controller.pending = None
                        package_rows, information = work(discover)
                        rows = [r['package'] for r in package_rows]
                        row_index = {r['package']: r for r in package_rows}
                        selected.intersection_update(rows)
                        previous_filter = None
                        message = 'Catalog refreshed.'
                elif key == ord('?'): ask('\n'.join(k + ': ' + v[2] for k, v in OPTIONS.items()))
                elif key == ord('z'):
                    if ask('Discard pending edits? type yes:') == 'yes': controller.pending = None
                elif key == ord('q'):
                    if not controller.pending or ask('Discard pending edits? type yes:') == 'yes': return
            except (RuntimeError, ValueError, OSError) as error:
                message = str(error).strip() or type(error).__name__ + ': operation failed'
    try: curses.wrapper(run)
    finally: executor.shutdown(wait=True, cancel_futures=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('packages', 'show', 'configure', 'disable', 'adopt', 'setup-workspaces', 'workspaces', 'tui', 'gui', 'recover', '_apply', '_workspace'))
    parser.add_argument('packages', nargs='*')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--search', default='')
    parser.add_argument('--filter', choices=('all', 'installed', 'available', 'configured'), default='all')
    parser.add_argument('--set', action='append', default=[], metavar='OPTION=VALUE')
    parser.add_argument('--workspace-action', choices=('list', 'status', 'pack', 'unpack', 'gc'), default='list')
    parser.add_argument('--name')
    parser.add_argument('--profile')
    parser.add_argument('--root', default='/var/cache/openmktr/workspaces')
    parser.add_argument('--compression', choices=('xz', 'zstd'), default='xz')
    parser.add_argument('--idle-seconds', type=float, default=3600)
    args = parser.parse_args()
    backend = None if args.action in ('workspaces', '_workspace') else Backend()
    if args.action == '_workspace':
        if os.geteuid() != 0: raise RuntimeError('workspace helper requires root')
        print(workspace(json.loads(sys.stdin.read(65536)), elevate=False), end='')
    elif args.action == 'recover':
        if os.geteuid() != 0:
            raise RuntimeError('run omktr-manager recover with administrative privileges')
        backend.recover()
    elif args.action == '_apply':
        if os.geteuid() != 0:
            raise RuntimeError('privileged helper requires root')
        plan = json.loads(sys.stdin.read(1024*1024))
        if plan.get('action') not in ('configure', 'disable', 'adopt', 'setup-workspaces'):
            raise ValueError('invalid helper action')
        backend.apply(plan)
    elif args.action in ('gui', 'tui'):
        (gui if args.action == 'gui' else tui)(backend)
    elif args.action == 'workspaces':
        print(workspace(dict(action=args.workspace_action, root=args.root, name=args.name, profile=args.profile, compression=args.compression, idle_seconds=args.idle_seconds)), end='')
    elif args.action in ('packages', 'show'):
        if args.action == 'show':
            result = [backend.show(p) for p in args.packages]
        else:
            result = []
            for row in backend.portage.packages():
                if args.search.lower() not in row['package'].lower(): continue
                info = backend.show(row['package'])
                if args.filter in ('installed', 'available') and not row[args.filter]: continue
                if args.filter == 'configured' and info['state'] == 'disabled': continue
                result.append({**row, **info})
        print(json.dumps(result, indent=2) if args.json else '\n'.join(r['package'] + ': ' + r['state'] for r in result))
    else:
        values = {}
        for item in args.set:
            name, value = item.split('=', 1)
            if name not in OPTIONS: raise ValueError('unknown option: ' + name)
            if type(OPTIONS[name][0]) is bool:
                if value not in ('true', 'false'): raise ValueError('boolean must be true or false')
                value = value == 'true'
            values[name] = value
        if args.action != 'setup-workspaces' and not args.packages:
            parser.error('select at least one package')
        plan = backend.setup({'root': args.root}) if args.action == 'setup-workspaces' else backend.plan(args.action, args.packages, values)
        print(diff(plan), end='')
        if not args.dry_run: save(backend, plan)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print('omktr-manager: ' + str(error), file=sys.stderr)
        sys.exit(1)
