#!/usr/bin/env python3
"""Local LibreOffice comparison. Fork contribution, MPL-2.0 (2026)."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time


def elapsed_clock():
    # NTP can slew CLOCK_MONOTONIC in WSL. Measure elapsed time against the
    # unslewed local clock and retain adjusted/realtime observations separately.
    return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)


def clock_observation():
    return dict(realtime_ns=time.time_ns(), monotonic_ns=time.monotonic_ns(),
                monotonic_raw_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW))


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--omktr', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--configuration', type=Path, required=True,
                        help='JSON containing the recorded configure_options list')
    parser.add_argument('--make', default='make')
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--ninja-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=32)
    parser.add_argument('--lanes', nargs='+', choices=['gnu', 'direct', 'safe', 'aggressive'], default=['gnu', 'direct', 'safe'])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--pdf-module-path', type=Path)
    args = parser.parse_args()
    if args.repeats < 1 or args.jobs < 1:
        parser.error("--repeats and --jobs must be positive")
    root = args.workspace.resolve()
    root.mkdir(parents=True, exist_ok=True)
    args.report = args.report.resolve()
    args.report.mkdir(parents=True, exist_ok=True)
    archive = args.archive.resolve()
    args.omktr = args.omktr.resolve()
    old = json.loads(args.configuration.read_text())
    options = [re.sub(r'--with-parallelism=\d+', '--with-parallelism=' + str(args.jobs), s) for s in old['configure_options']]
    env = dict(os.environ, CC=args.cc, CXX=args.cxx, CCACHE_DISABLE='1', LC_ALL='C', TZ='UTC')
    if args.ninja_dir:
        env['PATH'] = str(args.ninja_dir.resolve()) + os.pathsep + env.get('PATH', os.defpath)
    make = shutil.which(args.make, path=env.get('PATH', os.defpath))
    if not make:
        parser.error('external GNU Make executable not found')
    for key in list(env):
        if key.startswith(('KATI_', 'OMKTR_', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')) or key == 'MAKE':
            env.pop(key)
    report = dict(status='preparing', jobs=args.jobs, repeats=args.repeats, configure_options=options,
                  environment={k: env[k] for k in ('PATH', 'CC', 'CXX', 'CCACHE_DISABLE', 'LC_ALL', 'TZ')},
                  archive_sha256=digest(archive), binary_sha256=digest(args.omktr), runs={}, lanes={})
    assert report['archive_sha256'] == '21b38b7f429d305b229fb70f3294dd6ccd803f52dad0463d0923cdf143e9c8bd'
    if args.resume:
        previous = json.loads((root / 'report.json').read_text())
        for field in ('jobs', 'configure_options', 'archive_sha256', 'binary_sha256', 'environment'):
            if previous[field] != report[field]:
                raise RuntimeError('Resume identity differs from recorded campaign: ' + field)
        report = previous
    pdf_extract = ('import sys; from pathlib import Path; '
                   + ('sys.path.insert(0,' + repr(str(args.pdf_module_path)) + '); '
                      if args.pdf_module_path else '')
                   + 'from pypdf import PdfReader; '
                   'r=PdfReader(sys.argv[1]); '
                   'Path(sys.argv[2]).write_text("\\n".join(p.extract_text() for p in r.pages))')
    subprocess.run([sys.executable, '-c', pdf_extract.split('r=PdfReader')[0]], check=True)

    def save():
        payload = json.dumps(report, indent=2)
        for folder in (root, args.report):
            temp = folder / 'report.tmp'
            temp.write_text(payload)
            temp.replace(folder / 'report.json')

    def resources(pid):
        mem = {k: int(v.split()[0]) for k, v in (line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())}
        procs = {}
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                stat = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
                procs[int(entry.name)] = (int(stat[1]), int(stat[11]) + int(stat[12]), int(stat[21]) * os.sysconf('SC_PAGE_SIZE'))
            except (OSError, ValueError, IndexError):
                pass
        descendants = {pid}
        while True:
            new = {p for p, v in procs.items() if v[0] in descendants}
            if new <= descendants:
                break
            descendants |= new
        selected = [procs[p] for p in descendants if p in procs]
        io = {'read_bytes': 0, 'write_bytes': 0}
        for p in descendants:
            try:
                vals = dict(line.split(': ', 1) for line in Path('/proc', str(p), 'io').read_text().splitlines())
                for key in io:
                    io[key] += int(vals[key])
            except OSError:
                pass
        return dict(MemTotal=mem['MemTotal'], MemAvailable=mem['MemAvailable'], swap_used_kib=mem['SwapTotal']-mem['SwapFree'], process_count=len(selected), sampled_tree_rss_bytes=sum(v[2] for v in selected), live_cpu_ticks=sum(v[1] for v in selected), live_process_io=io, loadavg=Path('/proc/loadavg').read_text().strip())

    def run(label, command, cwd, extra=None):
        if args.resume and label in report['runs']:
            previous = report['runs'][label]
            if (previous['exit_code'] or previous['command'] != command or
                    previous['cwd'] != str(cwd) or
                    previous.get('environment_overrides') != extra):
                raise RuntimeError('Cannot reuse failed or different phase: ' + label)
            return previous
        report['status'] = label
        save()
        done = threading.Event()
        samples = []
        print('phase-start', label, flush=True)
        started = elapsed_clock()
        started_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
        with (root / (label + '.log')).open('w') as log:
            log.write(json.dumps(command) + '\n')
            log.flush()
            child = subprocess.Popen(command, cwd=cwd, env=dict(env, **(extra or {})), stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            def monitor():
                with (root / (label + '.resources.jsonl')).open('w') as stream:
                    while not done.is_set():
                        sample = dict(elapsed_seconds=elapsed_clock()-started, **clock_observation(), **resources(child.pid))
                        samples.append(sample)
                        stream.write(json.dumps(sample)+'\n')
                        stream.flush()
                        done.wait(2)
            thread = threading.Thread(target=monitor)
            thread.start()
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
            elapsed = elapsed_clock()-started
            done.set()
            thread.join()
        result = dict(command=command, cwd=str(cwd), started_utc=started_utc, environment_overrides=extra, exit_code=child.returncode, wall_seconds=elapsed, user_seconds=usage.ru_utime, system_seconds=usage.ru_stime, average_cpu_cores=(usage.ru_utime+usage.ru_stime)/elapsed, peak_individual_rss_kib=usage.ru_maxrss, sampled_peak_tree_rss_bytes=max((s['sampled_tree_rss_bytes'] for s in samples), default=0), minimum_mem_available_kib=min((s['MemAvailable'] for s in samples), default=0), maximum_swap_used_kib=max((s['swap_used_kib'] for s in samples), default=0), log=label+'.log')
        offsets = [(s['realtime_ns']-s['monotonic_raw_ns'])/1e9 for s in samples]
        jumps = [b-a for a, b in zip(offsets, offsets[1:]) if abs(b-a) > 0.05]
        result['elapsed_clock'] = 'CLOCK_MONOTONIC_RAW'
        result['clock_validation'] = dict(realtime_discontinuities_seconds=jumps,
                                           maximum_sample_offset_span_seconds=max(offsets, default=0)-min(offsets, default=0))
        report['runs'][label] = result
        with (root/'commands.jsonl').open('a') as ledger:
            ledger.write(json.dumps(result)+'\n')
        save()
        print(label, result['exit_code'], round(elapsed, 2), flush=True)
        if jumps:
            raise RuntimeError('Clock discontinuities invalidate phase: ' + label)
        return result

    report['versions'] = {name: subprocess.check_output(cmd, env=env, text=True, stderr=subprocess.STDOUT) for name, cmd in {'omktr':[str(args.omktr),'--version'], 'make':[make,'--version'], 'clang':[args.cc,'--version'], 'ninja':['ninja','--version'], 'kernel':['uname','-a'], 'cpu':['lscpu']}.items()}
    save()
    for lane in args.lanes:
        assert shutil.disk_usage(root).free > 60*1024**3, 'Insufficient free disk'
        base = root/lane
        if base.exists():
            if not args.resume or lane not in report['lanes']:
                raise RuntimeError('Existing lane requires recorded resume state')
            data = report['lanes'][lane]
            expected = digest(args.omktr) if lane != 'gnu' else digest(make)
            if data['binary_sha256'] != expected or lane+'-build' not in report['runs']:
                raise RuntimeError('Existing lane is incomplete or binary changed')
        else:
            base.mkdir(exist_ok=False)
            start = elapsed_clock()
            with tarfile.open(archive) as tar:
                tar.extractall(base, filter='data')
            data = report['lanes'][lane] = dict(extract_seconds=elapsed_clock()-start,
                                              binary_sha256=digest(args.omktr) if lane != 'gnu' else digest(make))
        source = base/'libreoffice-25.2.7.2'
        build = base/'build'
        build.mkdir(exist_ok=args.resume)
        total_start = elapsed_clock()
        configured = run(lane+'-configure', [str(source/'configure')]+options, build)
        if configured['exit_code']:
            continue
        normalized = (build/'config_host.mk').read_text().replace(str(base), '<LANE>')
        (root/(lane+'-config-normalized.mk')).write_text(normalized)
        normalized_digest = hashlib.sha256(normalized.encode()).hexdigest()
        if data.get('normalized_config_sha256', normalized_digest) != normalized_digest:
            raise RuntimeError('Recorded lane configuration changed')
        data['normalized_config_sha256'] = normalized_digest
        langs = re.search(r'^ALL_LANGS=(.*)$', (build/'config_host_lang.mk').read_text(), re.M).group(1)
        tool = make if lane=='gnu' else str(args.omktr)
        prepared = run(lane+'-bootstrap', [tool, '-j'+str(args.jobs), 'MAKE='+tool, 'ALL_LANGS='+langs, 'bootstrap', 'fetch', 'install-gdb-printers'], build)
        if prepared['exit_code']:
            continue
        command = [tool]+([] if lane in ('gnu', 'direct') else ['build','--graph_optimizations='+lane])+['-j'+str(args.jobs),'-rs','-f',str(source/'Makefile.gbuild'),'MAKE='+tool,'ALL_LANGS='+langs,'build']
        extra = dict(SRCDIR=str(source),BUILDDIR=str(build),WORKDIR=str(build/'workdir'))
        built = run(lane+'-build', command, build, extra)
        if 'configure_through_build_seconds' not in data:
            data['configure_through_build_seconds'] = elapsed_clock()-total_start
        if built['exit_code']==0:
            if 'installed_before_repeat' not in data:
                data['installed_before_repeat'] = manifest(build/'instdir')
            initial_state = root/(lane+'-outputs-before-repeats.json')
            if args.resume and initial_state.exists():
                before = json.loads(initial_state.read_text())
            else:
                before = output_state(build)
                initial_state.write_text(json.dumps(before, indent=2))
            data['repeat_checks'] = []
            for repeat in range(1, args.repeats + 1):
                measured = run(lane+'-noop-'+str(repeat), command, build, extra)
                state_path = root/(lane+'-outputs-after-repeat-'+str(repeat)+'.json')
                if args.resume and state_path.exists():
                    after = json.loads(state_path.read_text())
                else:
                    after = output_state(build)
                    state_path.write_text(json.dumps(after, indent=2))
                changed = sorted(k for k in before.keys() | after.keys()
                                 if before.get(k) != after.get(k))
                metadata_only = [k for k in changed if
                                 k in ('instdir/program/setuprc', 'instdir/program/versionrc')
                                 and before.get(k) and after.get(k)
                                 and before[k]['identity'] == after[k]['identity']
                                 and before[k]['size'] == after[k]['size']
                                 and before[k]['inode'] == after[k]['inode']]
                blocking = sorted(set(changed) - set(metadata_only))
                data['repeat_checks'].append(dict(repeat=repeat, changed=changed,
                                                  metadata_only_configuration_rewrites=metadata_only,
                                                  blocking_changes=blocking,
                                                  exit_code=measured['exit_code']))
                before = after
                save()
                if measured['exit_code'] or blocking:
                    raise RuntimeError('Repeat changed outputs: ' + repr(blocking[:20]))
            data['installed_after_repeat'] = manifest(build/'instdir')
            smoke = base/'smoke'
            smoke.mkdir(exist_ok=args.resume)
            (smoke/'input.txt').write_text('Openmktr LibreOffice benchmark verification\n')
            (smoke/'input.csv').write_text('Name,Value\nBenchmark,42\n')
            (smoke/'input.fodp').write_text('<?xml version="1.0"?><office:document xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" office:version="1.2" office:mimetype="application/vnd.oasis.opendocument.presentation"><office:body><office:presentation><draw:page draw:name="Benchmark"><draw:frame svg:x="1cm" svg:y="1cm" svg:width="20cm" svg:height="5cm"><draw:text-box><text:p>Openmktr Impress verification</text:p></draw:text-box></draw:frame></draw:page></office:presentation></office:body></office:document>')
            soffice = str(build/'instdir/program/soffice')
            assert run(lane+'-version',[soffice,'--version'],build)['exit_code'] == 0
            for kind, ext in [('writer','txt'),('calc','csv'),('impress','fodp')]:
                out = smoke/kind
                out.mkdir(exist_ok=args.resume)
                run(lane+'-smoke-'+kind,['timeout','120',soffice,'-env:UserInstallation='+ (smoke/('profile-'+kind)).as_uri(),'--headless','--convert-to','pdf','--outdir',str(out),str(smoke/('input.'+ext))],build)
                pdf = out/'input.pdf'
                assert pdf.exists(), 'PDF conversion failed: ' + kind
                assert run(lane+'-pdf-'+kind,[sys.executable,'-c',pdf_extract,str(pdf),str(out/'content.txt')],build)['exit_code'] == 0
                expected = dict(writer=['Openmktr LibreOffice benchmark verification'],
                                calc=['Name', 'Value', 'Benchmark', '42'],
                                impress=['Openmktr Impress verification'])[kind]
                content = (out/'content.txt').read_text()
                assert all(text in content for text in expected), (kind, content)
            data['installed_manifest'] = manifest(build/'instdir')
            assert run(lane+'-libraries',['ldd',str(build/'instdir/program/soffice.bin')],build)['exit_code'] == 0
            assert 'not found' not in (root/(lane+'-libraries.log')).read_text()
            assert '25.2.7.2' in (root/(lane+'-version.log')).read_text()
        run(lane+'-source-integrity',['python3',str(Path(__file__).with_name('verify_release_archive.py')),str(archive),str(source)],build)
        save()
    report['comparison_lanes'] = args.lanes
    report['excluded_lanes'] = sorted(set(report['lanes']) - set(args.lanes))
    selected_runs = [v for k, v in report['runs'].items()
                     if any(k.startswith(lane+'-') for lane in args.lanes)]
    assert all(v['exit_code'] == 0 for v in selected_runs), 'Failed comparison command'
    report['status'] = 'builds-finished-cross-lane-verification-pending'
    save()


def output_state(build):
    paths = list((build/'instdir').rglob('*'))
    paths.extend((build/'workdir').rglob('*.o'))
    paths.extend((build/'workdir').rglob('*.a'))
    paths.extend((build/'workdir').rglob('*.zip'))
    result = {}
    for p in paths:
        if p.is_file() or p.is_symlink():
            st = p.lstat()
            result[str(p.relative_to(build))] = dict(
                mtime_ns=st.st_mtime_ns, size=st.st_size, inode=st.st_ino,
                identity=os.readlink(p) if p.is_symlink() else digest(p))
    return result


def manifest(root):
    return {str(p.relative_to(root)): ({'symlink':os.readlink(p)} if p.is_symlink() else {'bytes':p.stat().st_size,'sha256':digest(p)}) for p in sorted(root.rglob('*')) if p.is_file() or p.is_symlink()}


if __name__=='__main__':
    main()
