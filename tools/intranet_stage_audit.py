#!/usr/bin/env python3
"""Collect P9-P14 intranet evidence without interrupting a real testcase."""
import argparse
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys

from intranet_smoke_common import (ROOT, config_and_root, path_from,
                                    report_path, request, utc_now, write_report)


STAGE_TESTS = {
    'P10': ('test_batch_cancel',),
    'P11': ('test_sqlite_persistence', 'test_sqlite_worker'),
    'P12': ('test_worker_recovery', 'test_recovery_process'),
    'P13': ('test_worker_distribution', 'test_multislot_process', 'test_slot_identity'),
    'P14': ('test_regression_service',),
}


def read_json(path):
    if not path.is_file():
        return None
    with path.open(encoding='utf-8') as stream:
        return json.load(stream)


def compact_smoke(config, filename):
    path = report_path(config, filename)
    try:
        data = read_json(path)
    except (OSError, ValueError) as error:
        return {'status': 'INVALID', 'error': str(error), 'path': str(path)}
    if data is None:
        return {'status': 'NOT_RUN', 'path': str(path)}
    result = {'status': data.get('verdict', 'INCOMPLETE'), 'path': str(path),
              'revision': data.get('revision'), 'finished_at': data.get('finished_at')}
    if data.get('side') == 'server':
        result['tasks'] = [{'task_id': task.get('task_id'),
                            'status': task.get('status'),
                            'worker_id': task.get('worker_id'),
                            'case_status': (task.get('execution') or {}).get('case_status')}
                           for task in data.get('tasks', [])]
        result['worker_ids'] = data.get('worker_ids')
        result['machine_ids'] = data.get('machine_ids')
        result['overlap_ms'] = data.get('overlap_ms')
    else:
        result['worker_ids'] = data.get('worker_ids') or [data.get('worker_id')]
        result['checkout_complete'] = data.get('checkout_complete')
    if data.get('error'):
        result['error'] = data['error']
    return result


def run_offline(config, report):
    intranet_ctest = Path('/home/xshare/scripts/bin/cmake-3.15.2/bin/ctest')
    ctest = str(intranet_ctest) if intranet_ctest.is_file() else shutil.which('ctest')
    report['offline_ctest'] = ctest
    build = ROOT / 'build'
    if not ctest:
        report['offline_error'] = 'ctest not found in PATH or the intranet CMake directory'
        return
    if not (build / 'CTestTestfile.cmake').is_file():
        report['offline_error'] = 'build/CTestTestfile.cmake missing; configure BUILD_TESTING=ON first'
        return
    logs = report_path(config, 'stage-test-logs')
    logs.mkdir(parents=True, exist_ok=True)
    report['offline_tests'] = {}
    for stage, tests in STAGE_TESTS.items():
        report['offline_tests'][stage] = {}
        for name in tests:
            log = logs / (name + '.log')
            try:
                if name not in ('test_sqlite_worker', 'test_recovery_process',
                                'test_multislot_process', 'test_slot_identity') and not (
                        build / 'bin' / name).is_file():
                    report['offline_tests'][stage][name] = {
                        'status': 'NOT_BUILT', 'detail': 'build the test target first'}
                    print('[STAGE-AUDIT] {} {} NOT_BUILT'.format(stage, name), flush=True)
                    continue
                inventory = subprocess.check_output([ctest, '-N', '-R',
                    '^' + name + '$'], cwd=str(build),
                    stderr=subprocess.STDOUT, timeout=15).decode('utf-8', 'replace')
                if 'Total Tests: 1' not in inventory:
                    raise RuntimeError('test not registered in build: ' + name)
                with log.open('w', encoding='utf-8') as stream:
                    completed = subprocess.run([ctest, '-R', '^' + name + '$',
                        '--output-on-failure'], cwd=str(build), stdout=stream,
                        stderr=subprocess.STDOUT, timeout=180, check=False)
                report['offline_tests'][stage][name] = {
                    'status': 'PASS' if completed.returncode == 0 else 'FAIL',
                    'exit_code': completed.returncode, 'log': str(log)}
            except (OSError, RuntimeError, subprocess.TimeoutExpired,
                    subprocess.CalledProcessError) as error:
                report['offline_tests'][stage][name] = {
                    'status': 'FAIL', 'error': str(error), 'log': str(log)}
            print('[STAGE-AUDIT] {} {} {}'.format(stage, name,
                report['offline_tests'][stage][name]['status']), flush=True)


def server_audit(config, report, run_tests):
    if run_tests:
        run_offline(config, report)
    report['real_smoke'] = {
        'P9_single_server': compact_smoke(config, 'server-report.json'),
        'P13_dual_server': compact_smoke(config, 'dual-server-report.json'),
    }
    single_path = report_path(config, 'server-report.json')
    single = read_json(single_path)
    if isinstance(single, dict) and single.get('verdict') == 'PASS':
        task_ids = [item.get('task_id') for item in single.get('tasks', [])]
        db = path_from(config, 'database.path')
        if db.is_file() and task_ids and all(type(value) is int for value in task_ids):
            try:
                # Read-only URI: do not create or modify the Server database.
                uri = 'file:{}?mode=ro'.format(db.as_posix())
                connection = sqlite3.connect(uri, uri=True, timeout=2)
                try:
                    rows = connection.execute('SELECT task_id,status,revision FROM tasks '
                        'WHERE task_id IN ({})'.format(','.join('?' for _ in task_ids)),
                        task_ids).fetchall()
                    report['P11_sqlite_evidence'] = {
                        'status': 'MATCH' if len(rows) == len(task_ids) and all(
                            row[1] == 'SUCCEEDED' and row[2] == single.get('revision')
                            for row in rows) else 'MISMATCH',
                        'task_ids': sorted(row[0] for row in rows), 'database': str(db)}
                finally:
                    connection.close()
            except (sqlite3.Error, OSError) as error:
                report['P11_sqlite_evidence'] = {'status': 'ERROR', 'detail': str(error)}
    report['requires_live_validation'] = {
        'P10': 'batch stop: queued cases cancel; active real case finishes normally',
        'P11': 'restart Server with preserved SQLite DB; IDs, results and batch state persist',
        'P12': 'Server restart/disconnect during a disposable run; same run_id, no duplicate execution',
        'P13': 'two hosts and slot count 3 to 5 with checkout reuse; inspect Worker report',
        'P14': 'QUERY_REGRESSION on a real completed batch, including pagination and outcome counts',
    }
    try:
        status = request(config, 'query_status', {})
        report['live_server'] = {'status': 'REACHABLE',
            'online_workers': [{'worker_id': w.get('worker_id'),
                                'machine_id': w.get('machine_id'),
                                'slot_id': w.get('slot_id'),
                                'status': w.get('status')}
                               for w in status.get('workers', [])
                               if w.get('status') == 'ONLINE']}
        if isinstance(single, dict) and single.get('verdict') == 'PASS':
            expected = set(item.get('task_id') for item in single.get('tasks', []))
            observed = set()
            query = {'revision': single['revision'], 'case': single['case'], 'limit': 50}
            pages = 0
            first = None
            while pages < 20:
                current = request(config, 'query_regression', query)
                if first is None:
                    first = current
                pages += 1
                observed.update(item.get('task_id') for item in current.get('items', []))
                if expected.issubset(observed) or not current.get('has_more'):
                    break
                cursor = current.get('next_before_task_id')
                if type(cursor) is not int or cursor <= 0:
                    raise ValueError('invalid QUERY_REGRESSION pagination cursor')
                query['before_task_id'] = cursor
            report['P14_regression_evidence'] = {
                'status': 'MATCH' if expected and expected.issubset(observed) else 'MISMATCH',
                'expected_task_ids': sorted(expected), 'total': first.get('total'),
                'counts': first.get('counts'), 'pages_checked': pages,
                'more_pages': current.get('has_more')}
    except (OSError, ValueError, RuntimeError) as error:
        report['live_server'] = {'status': 'NOT_REACHABLE', 'detail': str(error)}
    test_states = [item.get('status') for tests in
                   report.get('offline_tests', {}).values() for item in tests.values()]
    report['verdict'] = ('OFFLINE_CHECKS_FAILED' if report.get('offline_error') or
                         any(state != 'PASS' for state in test_states)
                         else 'EVIDENCE_COLLECTED')


def worker_audit(config, report):
    from slot_manager import slot_settings
    machine, slots, output = slot_settings(config)
    root = path_from(config, 'worker.slots_root')
    report['machine'] = machine
    report['slots'] = []
    try:
        status = request(config, 'query_status', {})
        workers = {w.get('worker_id'): w for w in status.get('workers', [])}
        report['server'] = 'REACHABLE'
    except (OSError, ValueError, RuntimeError) as error:
        workers = {}
        report['server'] = 'NOT_REACHABLE: ' + str(error)
    for slot in slots:
        worker_id = machine + '-' + slot
        checkout = root / worker_id / 'galaxcore'
        entry = {'worker_id': worker_id,
                 'checkout_complete': ((checkout / '.svn').is_dir() and
                                       (checkout / 'test2/run.sh').is_file()),
                 'server_status': workers.get(worker_id, {}).get('status', 'NOT_SEEN'),
                 'checkout': str(checkout)}
        report['slots'].append(entry)
    report['identity_file'] = str(output / '.machine-identity.json')
    report['real_smoke'] = {
        'P9_single_worker': compact_smoke(config, 'worker-report.json'),
        'P13_dual_worker': compact_smoke(config, 'dual-worker-report.json'),
        'P13_two_machine_worker': compact_smoke(config, 'two-machine-worker-report.json'),
    }
    report['verdict'] = 'EVIDENCE_COLLECTED'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--side', choices=('server', 'worker'), required=True)
    parser.add_argument('--run-offline-tests', action='store_true',
                        help='Server only: run P10-P14 local tests without touching real jobs')
    args = parser.parse_args()
    report = {'side': args.side, 'started_at': utc_now(), 'verdict': 'INCOMPLETE'}
    try:
        config = config_and_root()
        path = report_path(config, 'stage-{}-report.json'.format(args.side))
        if args.side == 'server':
            server_audit(config, report, args.run_offline_tests)
        elif args.run_offline_tests:
            raise ValueError('--run-offline-tests is for the Server side')
        else:
            worker_audit(config, report)
        report['finished_at'] = utc_now()
        write_report(path, report)
        print('[STAGE-AUDIT] evidence: ' + str(path), flush=True)
        return 1 if report['verdict'] in ('FAIL', 'OFFLINE_CHECKS_FAILED') else 0
    except (OSError, ValueError, RuntimeError) as error:
        report.update(verdict='FAIL', error=str(error), finished_at=utc_now())
        if 'config' in locals():
            path = report_path(config, 'stage-{}-report.json'.format(args.side))
            write_report(path, report)
        print('[STAGE-AUDIT] FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
