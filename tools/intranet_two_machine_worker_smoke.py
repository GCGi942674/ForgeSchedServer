#!/usr/bin/env python3
"""One-slot Worker launcher for a two-machine real PJTest acceptance run."""
import fcntl
import signal
import subprocess
import sys
import time

from intranet_smoke_common import (ROOT, config_and_root, number, report_path,
                                    request, tail, utc_now, write_report)
from intranet_worker_smoke import preflight, new_log_lines, observe_log

def run(config, report, path):
    setup = preflight(config)
    report.update(phase='preflight_ok', worker_id=setup['worker_id'],
                  machine_id=setup['machine_id'], slot_id=setup['slot_id'],
                  revision=setup['revision'], archive=setup['archive'],
                  task_results=[], slot_started=False, svn_checkout_started=False)
    write_report(path, report)
    console_path = report_path(config, 'two-machine-worker-console.log')
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
                observe_log(lines, report)
                if manager.poll() is not None:
                    raise RuntimeError('slot manager exited: {}'.format(manager.returncode))
                try:
                    status = request(config, 'query_status', {})
                    seen = True
                    lost_at = None
                    matching = [w for w in status.get('workers', [])
                                if w.get('worker_id') == setup['worker_id']]
                    if matching:
                        report['worker_status'] = matching[0].get('status')
                        report['phase'] = 'worker_registered'
                        report['slot_registered'] = True
                except (OSError, ValueError, RuntimeError):
                    if seen and lost_at is None:
                        lost_at = time.monotonic()
                report['server_seen'] = seen
                write_report(path, report)
                if seen and report['task_results']:
                    break
                if lost_at is not None and time.monotonic() - lost_at >= 15:
                    raise RuntimeError('Server disappeared before result')
                time.sleep(2)
            else:
                raise RuntimeError('deadline exceeded before local result')
            if len(report['task_results']) != 1:
                raise RuntimeError('expected exactly one local case result')
            item = report['task_results'][0]
            if (item['worker_id'] != setup['worker_id'] or
                    item['revision'] != setup['revision'] or
                    item['status'] != 'SUCCEEDED' or item['case_status'] != 'PASS'):
                raise RuntimeError('local case was not a clean PJTest PASS')
            task = request(config, 'query_task', {'task_id': item['task_id']})
            context = task.get('payload', {}).get('context', {})
            if (task.get('worker_id') != setup['worker_id'] or
                    task.get('status') != 'SUCCEEDED' or
                    not str(context.get('batch_id', '')).startswith('dual-smoke-')):
                raise RuntimeError('result does not belong to current two-machine test')
            report['batch_id'] = context['batch_id']
            manager.send_signal(signal.SIGTERM)
            manager.wait(timeout=60)
            if manager.returncode != 0:
                raise RuntimeError('slot manager stopped with {}'.format(manager.returncode))
            lines, offset = new_log_lines(console_path, offset)
            observe_log(lines, report)
            slot_root = setup['slot_root']
            report['checkout_complete'] = ((slot_root / '.svn').is_dir() and
                                           (slot_root / 'test2/run.sh').is_file())
            if not report['checkout_complete']:
                raise RuntimeError('complete SVN slot checkout not found')
            report.update(phase='complete', verdict='PASS', finished_at=utc_now())
            write_report(path, report)
            print('[TWO-MACHINE-WORKER] PASS; report: ' + str(path), flush=True)
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
        path = report_path(config, 'two-machine-worker-report.json')
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
            path = report_path(config, 'two-machine-worker-report.json')
            write_report(path, report)
            print('[TWO-MACHINE-WORKER] FAIL: {}; report: {}'.format(error, path),
                  file=sys.stderr)
        else:
            print('[TWO-MACHINE-WORKER] FAIL: ' + str(error), file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
