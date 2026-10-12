#!/usr/bin/env python3
"""Two-slot real PJTest acceptance: run on Worker after Server launcher."""
import fcntl
import json
import re
import shutil
import signal
import subprocess
import sys
import time

from intranet_smoke_common import (ROOT, config_and_root, number, path_from,
                                    report_path, request, tail, utc_now, write_report)
from intranet_worker_smoke import new_log_lines
from slot_manager import slot_settings

def preflight(config):
    machine, slots, _output = slot_settings(config)
    if len(slots) != 2:
        raise ValueError('dual-slot smoke requires worker.slots=2')
    if number(config, 'smoke.run_count', 2, 1, 3) != 2:
        raise ValueError('dual-slot smoke requires smoke.run_count=2')
    revision = config.get('smoke.revision', '').strip()
    if not re.fullmatch(r'[0-9]{1,20}', revision):
        raise ValueError('configure smoke.revision to match Server')
    if not config.get('worker.svn_url', '').strip() or not shutil.which('svn'):
        raise ValueError('configure worker.svn_url and install svn')
    if config.get('worker.checkout_parallelism', '1') != '1':
        raise ValueError('worker.checkout_parallelism must be 1')
    root = path_from(config, 'worker.artifact_root')
    archive = next((root / name for name in
                    ('GalaxCore_{}.zip'.format(revision),
                     'Galaxcore_{}.zip'.format(revision))
                    if (root / name).is_file()), None)
    if archive is None:
        raise ValueError('selected GalaxCore ZIP is missing under worker.artifact_root')
    if not config.get('network.server_ip', '').strip():
        raise ValueError('configure network.server_ip')
    if not (ROOT / 'worker' / 'slot_manager.py').is_file():
        raise ValueError('worker/slot_manager.py is missing')
    ids = [machine + '-' + slot for slot in slots]
    roots = [path_from(config, 'worker.slots_root') / worker_id / 'galaxcore'
             for worker_id in ids]
    return dict(machine=machine, slots=slots, ids=ids, roots=roots,
                revision=revision, archive=archive.name)

def observe(lines, report):
    for line in lines:
        line = line.strip()
        if '[MANAGER] started ' in line:
            for slot in report['slot_ids']:
                if '[MANAGER] started {} '.format(slot) in line:
                    report['slots_started'][slot] = True
        if '[WORKER] SVN checkout:' in line:
            report['svn_checkout_started'] = True
        if 'quarantined' in line:
            report['manager_warning'] = 'slot quarantined'
        if not line.startswith('{'):
            continue
        try:
            result = json.loads(line)
        except ValueError:
            continue
        if not isinstance(result, dict) or 'task_id' not in result or 'status' not in result:
            continue
        report['task_results'].append({key: result.get(key) for key in
            ('task_id', 'status', 'reason', 'case_status', 'case_reason',
             'exit_code', 'revision', 'worker_id')})
        print('[DUAL-WORKER] task {}: {} on {}'.format(
            result['task_id'], result['status'], result.get('worker_id')), flush=True)

def run(config, report, path):
    setup = preflight(config)
    report.update(phase='preflight_ok', machine_id=setup['machine'],
                  slot_ids=setup['slots'], worker_ids=setup['ids'],
                  revision=setup['revision'], archive=setup['archive'],
                  task_results=[], slots_started={slot: False for slot in setup['slots']},
                  svn_checkout_started=False)
    write_report(path, report)
    console_path = report_path(config, 'dual-worker-console.log')
    with console_path.open('w', encoding='utf-8') as console:
        manager = subprocess.Popen([sys.executable, '-B', '-u',
            str(ROOT / 'worker' / 'slot_manager.py')], cwd=str(ROOT),
            stdout=console, stderr=subprocess.STDOUT, close_fds=True)
        report.update(phase='waiting_for_server', manager_pid=manager.pid)
        write_report(path, report)
        offset = 0
        seen = False
        lost_at = None
        deadline = time.monotonic() + number(config, 'smoke.connect_wait_seconds',
             1200, 10, 86400) + number(config, 'smoke.wait_seconds', 12000, 30, 172800)
        try:
            while time.monotonic() < deadline:
                lines, offset = new_log_lines(console_path, offset)
                observe(lines, report)
                if manager.poll() is not None:
                    raise RuntimeError('slot manager exited: {}'.format(manager.returncode))
                try:
                    status = request(config, 'query_status', {})
                    seen = True
                    lost_at = None
                    online = {w.get('worker_id') for w in status.get('workers', [])
                              if w.get('status') == 'ONLINE'}
                    report['registered_ids'] = sorted(online.intersection(set(setup['ids'])))
                    if len(report['registered_ids']) == 2:
                        report['phase'] = 'slots_registered'
                except (OSError, ValueError, RuntimeError):
                    if seen and lost_at is None:
                        lost_at = time.monotonic()
                report['server_seen'] = seen
                write_report(path, report)
                if seen and len(report['task_results']) >= 2:
                    break
                if lost_at is not None and time.monotonic() - lost_at >= 15:
                    raise RuntimeError('Server disappeared before results')
                time.sleep(2)
            else:
                raise RuntimeError('deadline exceeded before two results')
            if len(report['task_results']) != 2 or any(
                    item['status'] != 'SUCCEEDED' or item['case_status'] != 'PASS'
                    for item in report['task_results']):
                raise RuntimeError('Worker did not report exactly two PJTest PASS cases')
            if {item['worker_id'] for item in report['task_results']} != set(setup['ids']):
                raise RuntimeError('each slot must execute exactly one case')
            manager.send_signal(signal.SIGTERM)
            manager.wait(timeout=60)
            if manager.returncode != 0:
                raise RuntimeError('slot manager stopped with {}'.format(manager.returncode))
            lines, offset = new_log_lines(console_path, offset)
            observe(lines, report)
            report['checkout_complete'] = {
                worker_id: (root / '.svn').is_dir() and (root / 'test2/run.sh').is_file()
                for worker_id, root in zip(setup['ids'], setup['roots'])}
            if not all(report['checkout_complete'].values()):
                raise RuntimeError('complete SVN checkout missing for a slot')
            report.update(phase='complete', verdict='PASS', finished_at=utc_now())
            write_report(path, report)
            print('[DUAL-WORKER] PASS; report: ' + str(path), flush=True)
        finally:
            if manager.poll() is None:
                manager.send_signal(signal.SIGTERM)
                try:
                    manager.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    report['manager_left_running'] = True
                    report['note'] = 'Manager left running to protect active case'
            if report.get('verdict') != 'PASS':
                report['worker_console_tail'] = tail(console_path)
            write_report(path, report)

def main():
    config = None
    report = {'side': 'worker', 'phase': 'starting', 'verdict': 'INCOMPLETE',
              'started_at': utc_now()}
    try:
        config = config_and_root()
        path = report_path(config, 'dual-worker-report.json')
        lock_path = report_path(config, '.worker-smoke.lock')
        with lock_path.open('a+') as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            report['lock_acquired'] = True
            write_report(path, report)
            run(config, report, path)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired,
            KeyboardInterrupt) as error:
        report.update(verdict='FAIL', error=str(error), finished_at=utc_now())
        if config is not None and report.get('lock_acquired'):
            path = report_path(config, 'dual-worker-report.json')
            write_report(path, report)
            print('[DUAL-WORKER] FAIL: {}; report: {}'.format(error, path),
                  file=sys.stderr)
        else:
            print('[DUAL-WORKER] FAIL: ' + str(error), file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
