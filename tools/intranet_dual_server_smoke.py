#!/usr/bin/env python3
"""Two-slot real PJTest acceptance: run on the Server before Worker launcher."""
import fcntl
import json
import os
import signal
import subprocess
import sys
import time
import uuid

from intranet_smoke_common import (ROOT, TERMINAL, config_and_root, number,
                                    report_path, request, tail, utc_now, write_report)
from intranet_server_smoke import preflight, wait_for_server

def expected_ids(config):
    raw = config.get('smoke.expected_worker_ids', '').strip()
    if not raw:
        return []
    ids = [part.strip() for part in raw.split(',')]
    if len(ids) != 2 or not all(ids) or ids[0] == ids[1]:
        raise ValueError('smoke.expected_worker_ids must contain two distinct full Worker IDs')
    return ids

def wait_for_slots(config, child, ids):
    deadline = time.monotonic() + number(config, 'smoke.connect_wait_seconds', 1200, 10, 86400)
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError('Server exited while waiting for two slots')
        status = request(config, 'query_status', {})
        online = {w['worker_id']: w for w in status.get('workers', [])
                  if w.get('status') == 'ONLINE'}
        if len(online) > 2:
            raise RuntimeError('this two-task test needs exactly two ONLINE slots; found {}'
                               .format(len(online)))
        selected = ids or sorted(online)
        if len(selected) == 2 and all(
                worker_id in online and online[worker_id].get('capacity') == 1
                and online[worker_id].get('used') == 0 for worker_id in selected):
            machines = {worker_id: online[worker_id].get('machine_id')
                        for worker_id in selected}
            if config.get('smoke.require_distinct_machines', '0').strip() == '1':
                if not all(machines.values()) or len(set(machines.values())) != 2:
                    raise RuntimeError('expected two distinct registered machine IDs')
            return selected, machines
        time.sleep(2)
    raise RuntimeError('two idle ONLINE slots did not register before deadline')

def run(config, report, path):
    setup = preflight(config)
    if setup['run_count'] != 2:
        raise ValueError('dual-slot smoke requires smoke.run_count=2')
    ids = expected_ids(config)
    report.update(phase='preflight_ok', revision=setup['revision'],
                  archive=setup['archive'], case=setup['case'], flow=setup['flow'],
                  worker_ids=ids, flow_config=setup['flow_config'], tasks=[])
    write_report(path, report)
    console_path = report_path(config, 'dual-server-console.log')
    with console_path.open('w', encoding='utf-8') as console:
        child = subprocess.Popen([setup['binary']], cwd=str(ROOT), stdout=console,
                                 stderr=subprocess.STDOUT, close_fds=True)
        report.update(server_pid=child.pid, phase='server_starting')
        write_report(path, report)
        try:
            initial = wait_for_server(config, child, 30)
            active = sum(int(initial.get(key, 0)) for key in
                         ('pending', 'queued', 'assigned', 'running'))
            if active:
                raise RuntimeError('Server has {} unfinished Tasks; none submitted'.format(active))
            report['phase'] = 'waiting_for_slots'
            write_report(path, report)
            print('[DUAL-SERVER] waiting for two idle slots', flush=True)
            ids, machines = wait_for_slots(config, child, ids)
            report['worker_ids'] = ids
            report['machine_ids'] = machines
            report['phase'] = 'slots_ready'
            write_report(path, report)
            batch_id = 'dual-smoke-' + uuid.uuid4().hex
            report['batch_id'] = batch_id
            for index in range(2):
                body = {'task_type': 'REGRESSION', 'target': setup['target'],
                        'revision': setup['revision'], 'priority': 0,
                        'submission_key': '{}-{}'.format(batch_id, index + 1),
                        'payload': {'spec_version': 2, 'case': setup['case'],
                                    'flow': setup['flow'],
                                    'timeout_seconds': number(config, 'smoke.case_timeout_seconds',
                                                              5400, 1, 86400),
                                    'flow_config': setup['flow_config'],
                                    'context': {'suite': 'intranet_dual_smoke',
                                                'template': setup['flow'],
                                                'batch_id': batch_id}}}
                task_id = request(config, 'submit_task', body)['task_id']
                report['tasks'].append({'task_id': task_id, 'status': 'SUBMITTED'})
                write_report(path, report)
                print('[DUAL-SERVER] submitted task {}'.format(task_id), flush=True)
            report['phase'] = 'waiting_for_results'
            write_report(path, report)
            deadline = time.monotonic() + number(config, 'smoke.wait_seconds',
                                                 12000, 30, 172800)
            last = None
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError('Server exited during test')
                current = [request(config, 'query_task', {'task_id': item['task_id']})
                           for item in report['tasks']]
                compact = [(task['task_id'], task['status']) for task in current]
                if compact != last:
                    print('[DUAL-SERVER] ' + ', '.join('{}={}'.format(*row)
                                                    for row in compact), flush=True)
                    last = compact
                for item, task in zip(report['tasks'], current):
                    item.update(status=task['status'], worker_id=task.get('worker_id'),
                                retry_count=task.get('retry_count'),
                                run_id=task.get('run_id'),
                                started_at_ms=task.get('started_at_ms'),
                                finished_at_ms=task.get('finished_at_ms'),
                                execution=task.get('execution'))
                write_report(path, report)
                if all(task['status'] in TERMINAL for task in current):
                    break
                time.sleep(2)
            else:
                raise RuntimeError('deadline exceeded while waiting for Task results')
            tasks = report['tasks']
            if any(t['status'] != 'SUCCEEDED' or t['retry_count'] != 0 or
                   (t.get('execution') or {}).get('case_status') != 'PASS' or
                   t.get('worker_id') not in ids or not t.get('run_id')
                   for t in tasks):
                raise RuntimeError('Task did not return clean PJTest PASS')
            if len({t['worker_id'] for t in tasks}) != 2:
                raise RuntimeError('both Tasks ran on the same slot')
            if len({t['run_id'] for t in tasks}) != 2:
                raise RuntimeError('duplicate run ID')
            if not all(t.get('started_at_ms') and t.get('finished_at_ms') for t in tasks):
                raise RuntimeError('missing Task time bounds')
            overlap = min(t['finished_at_ms'] for t in tasks) - max(
                t['started_at_ms'] for t in tasks)
            report['overlap_ms'] = overlap
            write_report(path, report)
            if overlap <= 0:
                raise RuntimeError('Task execution intervals did not overlap')
            print('[DUAL-SERVER] both PASS; overlap={} ms; waiting for drain'.format(
                overlap), flush=True)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                status = request(config, 'query_status', {})
                selected = {w['worker_id']: w for w in status.get('workers', [])
                            if w.get('worker_id') in ids}
                if all(worker_id not in selected or
                       selected[worker_id].get('status') == 'OFFLINE'
                       for worker_id in ids):
                    report['workers_drained'] = True
                    break
                time.sleep(2)
            if not report.get('workers_drained'):
                raise RuntimeError('slots did not drain; Server left available')
            report.update(phase='complete', verdict='PASS', finished_at=utc_now())
            write_report(path, report)
            print('[DUAL-SERVER] PASS; report: ' + str(path), flush=True)
        finally:
            active = [t['task_id'] for t in report['tasks']
                      if t.get('status') not in TERMINAL]
            if child.poll() is None and (active or
                                        (report['tasks'] and not report.get('workers_drained'))):
                report['server_left_running'] = True
                report['note'] = 'Server kept alive for active Tasks or slot inspection'
            elif child.poll() is None:
                child.send_signal(signal.SIGTERM)
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    report['server_left_running'] = True
            if report.get('verdict') != 'PASS':
                report['server_console_tail'] = tail(console_path)
            write_report(path, report)

def main():
    config = None
    report = {'side': 'server', 'phase': 'starting', 'verdict': 'INCOMPLETE',
              'started_at': utc_now()}
    try:
        config = config_and_root()
        path = report_path(config, 'dual-server-report.json')
        lock_path = report_path(config, '.server-smoke.lock')
        with lock_path.open('a+') as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            report['lock_acquired'] = True
            write_report(path, report)
            run(config, report, path)
        return 0
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        report.update(verdict='FAIL', error=str(error), finished_at=utc_now())
        if config is not None and report.get('lock_acquired'):
            path = report_path(config, 'dual-server-report.json')
            write_report(path, report)
            print('[DUAL-SERVER] FAIL: {}; report: {}'.format(error, path),
                  file=sys.stderr)
        else:
            print('[DUAL-SERVER] FAIL: ' + str(error), file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
