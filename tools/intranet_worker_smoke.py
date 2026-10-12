#!/usr/bin/env python3
"""Launch one real PJTest slot and write a compact Worker-side report."""
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

from intranet_smoke_common import (ROOT, config_and_root, number, path_from,
                                    report_path, request, tail, utc_now, write_report)


def preflight(config):
    machine = config.get('worker.machine_id', '').strip()
    slots = [part.strip() for part in config.get('worker.slot_ids', '').split(',')]
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', machine):
        raise ValueError('configure worker.machine_id')
    if (len(slots) != 1 or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', slots[0]) or
            len(machine + '-' + slots[0]) > 64):
        raise ValueError('smoke test requires exactly one worker.slot_ids entry')
    revision = config.get('smoke.revision', '').strip()
    if not re.fullmatch(r'[0-9]{1,20}', revision):
        raise ValueError('configure smoke.revision to match the Server')
    if not config.get('worker.svn_url', '').strip() or not shutil.which('svn'):
        raise ValueError('configure worker.svn_url and install svn')
    if config.get('worker.checkout_parallelism', '1') != '1':
        raise ValueError('worker.checkout_parallelism must be 1')
    artifact_root = path_from(config, 'worker.artifact_root')
    archive = next((artifact_root / name for name in
                    ('GalaxCore_{}.zip'.format(revision), 'Galaxcore_{}.zip'.format(revision))
                    if (artifact_root / name).is_file()), None)
    if archive is None:
        raise ValueError('selected GalaxCore ZIP is missing under worker.artifact_root')
    if not config.get('network.server_ip', '').strip():
        raise ValueError('configure network.server_ip to the Server LAN address')
    slot_manager = ROOT / 'worker' / 'slot_manager.py'
    if not slot_manager.is_file():
        raise ValueError('worker/slot_manager.py is missing')
    return dict(worker_id=machine + '-' + slots[0], machine_id=machine,
                slot_id=slots[0], revision=revision, archive=archive.name,
                slot_root=path_from(config, 'worker.slots_root') /
                (machine + '-' + slots[0]) / 'galaxcore')


def new_log_lines(path, offset):
    with path.open(encoding='utf-8', errors='replace') as stream:
        stream.seek(offset)
        lines = stream.readlines()
        return lines, stream.tell()


def observe_log(lines, report):
    for line in lines:
        line = line.strip()
        if '[MANAGER] started ' in line:
            report['slot_started'] = True
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
        print('[WORKER-SMOKE] task {}: {}'.format(result['task_id'], result['status']), flush=True)


def run(config, report, path):
    setup = preflight(config)
    report.update(phase='preflight_ok', worker_id=setup['worker_id'],
                  machine_id=setup['machine_id'], slot_id=setup['slot_id'],
                  revision=setup['revision'], archive=setup['archive'],
                  task_results=[], slot_started=False, svn_checkout_started=False)
    write_report(path, report)
    console_path = report_path(config, 'worker-console.log')
    with console_path.open('w', encoding='utf-8') as console:
        manager = subprocess.Popen([sys.executable, '-B', '-u',
            str(ROOT / 'worker' / 'slot_manager.py')], cwd=str(ROOT),
            stdout=console, stderr=subprocess.STDOUT, close_fds=True)
        report.update(phase='waiting_for_server', manager_pid=manager.pid)
        write_report(path, report)
        print('[WORKER-SMOKE] slot manager started; waiting for Server', flush=True)
        offset = 0
        server_seen = False
        server_lost_at = None
        expected = number(config, 'smoke.run_count', 2, 1, 3)
        deadline = time.monotonic() + number(config, 'smoke.connect_wait_seconds', 1200,
                                              10, 86400) + number(config, 'smoke.wait_seconds',
                                                                  12000, 30, 172800)
        try:
            while time.monotonic() < deadline:
                lines, offset = new_log_lines(console_path, offset)
                observe_log(lines, report)
                if manager.poll() is not None:
                    raise RuntimeError('slot manager exited: {}'.format(manager.returncode))
                try:
                    status = request(config, 'query_status', {})
                    server_seen = True
                    server_lost_at = None
                    matching = [w for w in status.get('workers', [])
                                if w.get('worker_id') == setup['worker_id']]
                    if matching:
                        report['worker_status'] = matching[0].get('status')
                        report['phase'] = 'worker_registered'
                        report['slot_registered'] = True
                except (OSError, ValueError, RuntimeError):
                    if server_seen and server_lost_at is None:
                        server_lost_at = time.monotonic()
                report['server_seen'] = server_seen
                write_report(path, report)
                if server_seen and len(report['task_results']) >= expected:
                    break
                if server_lost_at is not None and time.monotonic() - server_lost_at >= 15:
                    raise RuntimeError('Server disappeared before expected results')
                time.sleep(2)
            else:
                raise RuntimeError('deadline exceeded; Server or test completion not observed')
            manager.send_signal(signal.SIGTERM)
            manager.wait(timeout=60)
            if manager.returncode != 0:
                raise RuntimeError('slot manager stopped with {}'.format(manager.returncode))
            lines, offset = new_log_lines(console_path, offset)
            observe_log(lines, report)
            if len(report['task_results']) != expected or any(
                    item['status'] != 'SUCCEEDED' or item['case_status'] != 'PASS'
                    for item in report['task_results']):
                raise RuntimeError('Worker did not report exactly {} PJTest PASS cases'.format(expected))
            slot_root = setup['slot_root']
            report['checkout_complete'] = ((slot_root / '.svn').is_dir() and
                                           (slot_root / 'test2/run.sh').is_file())
            if not report['checkout_complete']:
                raise RuntimeError('complete SVN slot checkout not found')
            report.update(phase='complete', verdict='PASS', finished_at=utc_now())
            write_report(path, report)
            print('[WORKER-SMOKE] PASS; report: ' + str(path), flush=True)
        finally:
            if manager.poll() is None:
                manager.send_signal(signal.SIGTERM)
                try:
                    manager.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    report['manager_left_running'] = True
                    report['note'] = 'Manager left running to protect an active case'
            if report.get('verdict') != 'PASS':
                report['worker_console_tail'] = tail(console_path)
            write_report(path, report)


def main():
    config = None
    report = {'side': 'worker', 'phase': 'starting', 'verdict': 'INCOMPLETE',
              'started_at': utc_now()}
    try:
        config = config_and_root()
        path = report_path(config, 'worker-report.json')
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
            path = report_path(config, 'worker-report.json')
            write_report(path, report)
            print('[WORKER-SMOKE] FAIL: {}; report: {}'.format(error, path), file=sys.stderr)
        else:
            print('[WORKER-SMOKE] FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
