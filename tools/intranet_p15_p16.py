#!/usr/bin/env python3
"""Single-host, Python 3.6 compatible P15-P16 intranet acceptance.

Builds and runs isolated CTest cases, then optionally probes the running
Server read-only. Never submits tasks or touches a production Worker/DB.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback


ROOT = Path(__file__).resolve().parents[1]
STAGE_TESTS = {
    'P15': ('test_regression_service', 'test_sqlite_persistence',
            'test_worker_recovery', 'test_batch_cancel'),
    'P16': ('test_progress_service', 'test_protocol', 'test_client_cli'),
}
TESTS = tuple(dict.fromkeys(name for names in STAGE_TESTS.values()
                            for name in names))


def now():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def executable(name):
    internal = Path('/home/xshare/scripts/bin/cmake-3.15.2/bin') / name
    if internal.is_file():
        return str(internal)
    found = shutil.which(name)
    if not found:
        raise RuntimeError('{} is missing'.format(name))
    return found


def run_logged(argv, cwd, log, timeout):
    with log.open('w', encoding='utf-8') as stream:
        result = subprocess.run(argv, cwd=str(cwd), stdout=stream,
                                stderr=subprocess.STDOUT, timeout=timeout,
                                check=False)
    return result.returncode


def tail(path, lines=12):
    with path.open(encoding='utf-8', errors='replace') as stream:
        return [line.rstrip()[:300] for line in stream.readlines()[-lines:]]


def live_probe(report):
    sys.path.insert(0, str(ROOT / 'tools'))
    from intranet_smoke_common import config_and_root, request
    try:
        config = config_and_root()
        status = request(config, 'query_status', {})
    except (OSError, RuntimeError, ValueError) as error:
        report['live'] = {'status': 'NOT_RUNNING', 'detail': str(error)[:300]}
        return
    live = {'status': 'REACHABLE', 'worker_count': len(status.get('workers', [])),
            'cache_before': status.get('regression_cache')}
    try:
        revision = config.get('smoke.revision', '').strip()
        target = config.get('smoke.target', '').strip()
        if revision:
            filter_data = {'revision': revision, 'limit': 1}
            first = request(config, 'query_regression', filter_data)
            second = request(config, 'query_regression', filter_data)
            after = request(config, 'query_status', {}).get('regression_cache')
            live['cache_after'] = after
            live['cache_probe'] = ('HIT' if after and live['cache_before'] and
                after.get('hits', 0) > live['cache_before'].get('hits', 0)
                else 'NO_HIT_OBSERVED')
            live['regression_query_consistent'] = first == second
            if not target and first.get('items'):
                target = first['items'][0].get('target', '')
        else:
            live['cache_probe'] = 'NO_SMOKE_REVISION_CONFIGURED'
        if target:
            progress = request(config, 'query_progress', {'target': target,
                                                           'limit': 2})
            live['target'] = target
            live['total_revisions'] = progress.get('total_revisions')
            revisions = progress.get('revisions', [])
            live['revisions'] = [r.get('revision') for r in revisions]
            live['progress'] = 'OK'
            if len(revisions) > 1:
                earlier = revisions[1]['revision']
                later = revisions[0]['revision']
                rollback = request(config, 'query_rollback', {
                    'target': target, 'from_revision': earlier,
                    'to_revision': later, 'limit': 1})
                live['rollback'] = {'status': 'OK', 'from_revision': earlier,
                                    'to_revision': later,
                                    'total': rollback.get('total'),
                                    'from_available': rollback.get('from_available'),
                                    'to_available': rollback.get('to_available')}
            else:
                live['rollback'] = {'status': 'NO_COMPARABLE_REVISIONS'}
        else:
            live['progress'] = 'NO_TARGET_CONFIGURED_OR_DISCOVERED'
            live['rollback'] = {'status': 'NO_COMPARABLE_REVISIONS'}
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        live['status'] = 'QUERY_FAILED'
        live['detail'] = str(error)[:300]
    report['live'] = live


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-build', action='store_true',
                        help='use already configured and compiled build')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    report_dir = ROOT / 'intranet-report' / 'p15-p16'
    report_dir.mkdir(parents=True, exist_ok=True)
    report_file = report_dir / 'report.json'
    report = {'phase': 'start', 'started_at': now(),
              'verdict': 'INCOMPLETE', 'tests': {}}
    try:
        if not 1 <= args.jobs <= 32:
            raise ValueError('--jobs must be 1..32')
        build = ROOT / 'build'
        cmake, ctest = executable('cmake'), executable('ctest')
        if not args.no_build:
            print('[P15-P16] configuring with {}'.format(cmake), flush=True)
            configure_log = report_dir / 'configure.log'
            code = run_logged([cmake, '-S', str(ROOT), '-B', str(build),
                               '-DBUILD_TESTING=ON'], ROOT, configure_log, 180)
            if code:
                raise RuntimeError('CMake configure failed; see {}'.format(configure_log))
            print('[P15-P16] building {} test targets'.format(len(TESTS)), flush=True)
            build_log = report_dir / 'build.log'
            code = run_logged([cmake, '--build', str(build), '--target'] +
                              list(TESTS) + ['--parallel', str(args.jobs)],
                              ROOT, build_log, 900)
            if code:
                raise RuntimeError('build failed; see {}'.format(build_log))
        if not (build / 'CTestTestfile.cmake').is_file():
            raise RuntimeError('build not configured; omit --no-build')
        for name in TESTS:
            print('[P15-P16] running {}'.format(name), flush=True)
            log = report_dir / (name + '.log')
            code = run_logged([ctest, '-R', '^' + name + '$',
                               '--output-on-failure'], build, log, 120)
            output = log.read_text(encoding='utf-8', errors='replace')
            passed = code == 0 and '1/1 Test' in output and 'No tests were found' not in output
            report['tests'][name] = {'status': 'PASS' if passed else 'FAIL',
                                     'exit_code': code, 'log': str(log)}
            if not passed:
                report['tests'][name]['tail'] = tail(log)
            print('[P15-P16] {} {}'.format(name, report['tests'][name]['status']), flush=True)
        report['stages'] = {stage: ('PASS' if all(
            report['tests'][name]['status'] == 'PASS' for name in names) else 'FAIL')
            for stage, names in STAGE_TESTS.items()}
        live_probe(report)
        offline_ok = all(t['status'] == 'PASS' for t in report['tests'].values())
        if not offline_ok or report['live']['status'] == 'QUERY_FAILED':
            report['verdict'] = 'FAIL'
        elif report['live']['status'] == 'NOT_RUNNING':
            report['verdict'] = 'OFFLINE_PASS'
        elif report['live'].get('rollback', {}).get('status') == 'OK':
            report['verdict'] = 'PASS'
        else:
            report['verdict'] = 'API_PASS_NO_COMPARABLE_DATA'
        report['phase'] = 'complete'
        if report['live']['status'] == 'NOT_RUNNING':
            report['note'] = 'P15-P16 isolated tests passed; no running Server was inspected'
        elif report['live'].get('rollback', {}).get('status') == 'NO_COMPARABLE_REVISIONS':
            report['note'] = 'live API reachable; two comparable revisions unavailable'
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report['phase'] = 'error'
        report['verdict'] = 'FAIL'
        report['error'] = str(error)
        report['traceback_tail'] = traceback.format_exc().splitlines()[-5:]
    report['finished_at'] = now()
    with report_file.open('w', encoding='utf-8') as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write('\n')
    print('[P15-P16] {} report={}'.format(report['verdict'], report_file), flush=True)
    if report['verdict'] == 'FAIL':
        sys.exit(1)


if __name__ == '__main__':
    main()
