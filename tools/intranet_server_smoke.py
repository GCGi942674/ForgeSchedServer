#!/usr/bin/env python3
"""One-server/one-slot real PJTest acceptance run. Start this before Worker."""
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid

from intranet_smoke_common import (ROOT, TERMINAL, config_and_root, number,
                                    path_from, report_path, request, tail,
                                    utc_now, write_report)


def preflight(config):
    revision = config.get('smoke.revision', '').strip()
    case = config.get('smoke.case', '').strip()
    flow = config.get('smoke.flow', '').strip()
    target = config.get('smoke.target', '').strip()
    if not re.fullmatch(r'[0-9]{1,20}', revision):
        raise ValueError('configure smoke.revision as a ZIP revision number')
    if not case or case.startswith('/') or '\\' in case or not case.endswith('/run.tcl'):
        raise ValueError('configure smoke.case as a relative test2 path ending in /run.tcl')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', flow):
        raise ValueError('configure smoke.flow as a template name')
    if not target or len(target) > 128:
        raise ValueError('configure smoke.target')
    root = path_from(config, 'server.work_root')
    actual = (root / case).resolve(strict=True)
    if not actual.is_file() or actual.name != 'run.tcl':
        raise ValueError('smoke.case is not a run.tcl file')
    try:
        actual.relative_to(root)
    except ValueError:
        raise ValueError('smoke.case escapes server.work_root')
    if actual.relative_to(root).as_posix() != case:
        raise ValueError('smoke.case must be a canonical relative case path')
    artifacts = path_from(config, 'server.artifact_root')
    archive = next((artifacts / name for name in
                    ('GalaxCore_{}.zip'.format(revision), 'Galaxcore_{}.zip'.format(revision))
                    if (artifacts / name).is_file()), None)
    if archive is None:
        raise ValueError('selected GalaxCore ZIP is missing under server.artifact_root')
    template = path_from(config, 'server.templates_dir') / (flow + '.json')
    if not template.is_file() or template.is_symlink():
        raise ValueError('missing flow template ' + str(template))
    data = json.loads(template.read_text(encoding='utf-8'))
    flow_config = data.get('flow_config') if isinstance(data, dict) else None
    if (not isinstance(flow_config, dict) or not 1 <= len(flow_config) <= 64 or
            any(not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,63}', key) or
                type(value) is not int or not 0 <= value <= 1000000
                for key, value in flow_config.items())):
        raise ValueError('invalid flow_config in template')
    binary = path_from(config, 'tools.server_binary')
    if not binary.is_file() or not os.access(str(binary), os.X_OK):
        raise ValueError('build/bin/server is missing or not executable')
    return dict(revision=revision, case=case, flow=flow, target=target,
                flow_config=flow_config, archive=archive.name, binary=str(binary),
                run_count=number(config, 'smoke.run_count', 2, 1, 3))


def wait_for_server(config, child, seconds):
    deadline = time.monotonic() + seconds
    last = ''
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError('server exited during startup: {}'.format(child.returncode))
        try:
            return request(config, 'query_status', {})
        except (OSError, ValueError, RuntimeError) as error:
            last = str(error)
        time.sleep(1)
    raise RuntimeError('server did not become ready: ' + last)


def wait_for_worker(config, child, seconds, expected):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError('server exited while waiting for Worker')
        status = request(config, 'query_status', {})
        online = [w for w in status.get('workers', []) if w.get('status') == 'ONLINE']
        if expected:
            online = [w for w in online if w.get('worker_id') == expected]
        if len(online) == 1 and online[0].get('capacity') == 1 and online[0].get('used') == 0:
            if not expected and len([w for w in status.get('workers', [])
                                     if w.get('status') == 'ONLINE']) != 1:
                raise RuntimeError('multiple Workers online; set smoke.expected_worker_id')
            return online[0]['worker_id']
        time.sleep(2)
    raise RuntimeError('one idle ONLINE slot did not register before deadline')


def run(config, report, path):
    setup = preflight(config)
    report.update(phase='preflight_ok', revision=setup['revision'],
                  case=setup['case'], flow=setup['flow'],
                  flow_config=setup['flow_config'], archive=setup['archive'])
    write_report(path, report)
    console_path = report_path(config, 'server-console.log')
    with console_path.open('w', encoding='utf-8') as console:
        child = subprocess.Popen([setup['binary']], cwd=str(ROOT), stdout=console,
                                 stderr=subprocess.STDOUT, close_fds=True)
        report['server_pid'] = child.pid
        report['phase'] = 'server_starting'
        write_report(path, report)
        finished = False
        try:
            initial = wait_for_server(config, child, 30)
            active = sum(int(initial.get(key, 0)) for key in
                         ('pending', 'queued', 'assigned', 'running'))
            if active:
                raise RuntimeError('server already has {} unfinished tasks; no smoke tasks submitted'.format(active))
            print('[SERVER-SMOKE] Server ready; waiting for one Worker', flush=True)
            worker_id = wait_for_worker(config, child,
                number(config, 'smoke.connect_wait_seconds', 1200, 10, 86400),
                config.get('smoke.expected_worker_id', '').strip())
            report.update(phase='worker_ready', worker_id=worker_id)
            write_report(path, report)
            print('[SERVER-SMOKE] Worker online: ' + worker_id, flush=True)
            batch_id = 'smoke-' + uuid.uuid4().hex
            report['batch_id'] = batch_id
            report['tasks'] = []
            for index in range(setup['run_count']):
                body = {'task_type': 'REGRESSION', 'target': setup['target'],
                        'revision': setup['revision'], 'priority': 0,
                        'submission_key': '{}-{}'.format(batch_id, index + 1),
                        'payload': {'spec_version': 2, 'case': setup['case'],
                                    'flow': setup['flow'], 'timeout_seconds':
                                    number(config, 'smoke.case_timeout_seconds', 5400, 1, 86400),
                                    'flow_config': setup['flow_config'],
                                    'context': {'suite': 'intranet_smoke',
                                                'template': setup['flow'],
                                                'batch_id': batch_id}}}
                result = request(config, 'submit_task', body)
                report['tasks'].append({'task_id': result['task_id'], 'status': 'SUBMITTED'})
                write_report(path, report)
                print('[SERVER-SMOKE] submitted task {}'.format(result['task_id']), flush=True)
            report['phase'] = 'waiting_for_results'
            deadline = time.monotonic() + number(config, 'smoke.wait_seconds', 12000, 30, 172800)
            last = None
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError('server exited while tasks were running')
                current = [request(config, 'query_task', {'task_id': item['task_id']})
                           for item in report['tasks']]
                compact = [(task['task_id'], task['status']) for task in current]
                if compact != last:
                    print('[SERVER-SMOKE] ' + ', '.join('{}={}'.format(*item) for item in compact),
                          flush=True)
                    last = compact
                    for item, task in zip(report['tasks'], current):
                        item.update(task_id=task['task_id'], status=task['status'],
                                    worker_id=task.get('worker_id'),
                                    retry_count=task.get('retry_count'),
                                    run_id=task.get('run_id'),
                                    started_at_ms=task.get('started_at_ms'),
                                    finished_at_ms=task.get('finished_at_ms'),
                                    execution=task.get('execution'))
                    write_report(path, report)
                if all(task['status'] in TERMINAL for task in current):
                    finished = True
                    break
                time.sleep(2)
            if not finished:
                raise RuntimeError('deadline exceeded while waiting for task results')
            for item in report['tasks']:
                if (item['status'] != 'SUCCEEDED' or item['worker_id'] != worker_id or
                        item['retry_count'] != 0 or
                        (item.get('execution') or {}).get('case_status') != 'PASS'):
                    raise RuntimeError('task {} did not return a clean PJTest PASS'.format(item['task_id']))
            for first, second in zip(report['tasks'], report['tasks'][1:]):
                if not first.get('finished_at_ms') or not second.get('started_at_ms') or \
                        second['started_at_ms'] < first['finished_at_ms']:
                    raise RuntimeError('one-slot serialization could not be confirmed')
            print('[SERVER-SMOKE] Cases passed; waiting for Worker to drain', flush=True)
            offline_deadline = time.monotonic() + 120
            while time.monotonic() < offline_deadline:
                status = request(config, 'query_status', {})
                selected = [item for item in status.get('workers', [])
                            if item.get('worker_id') == worker_id]
                if not selected or selected[0].get('status') == 'OFFLINE':
                    report['worker_drained'] = True
                    break
                time.sleep(2)
            if not report.get('worker_drained'):
                raise RuntimeError('Worker did not drain; Server remains available for inspection')
            report.update(phase='complete', verdict='PASS', finished_at=utc_now())
            write_report(path, report)
            print('[SERVER-SMOKE] PASS; report: ' + str(path), flush=True)
        finally:
            # Never pretend to cancel a still-running real process. Keep Server
            # available for investigation/reconciliation if the run is unfinished.
            active_ids = [item['task_id'] for item in report.get('tasks', [])
                          if item.get('status') not in TERMINAL]
            if child.poll() is None and (active_ids or
                                        (report.get('tasks') and not report.get('worker_drained'))):
                report['server_left_running'] = True
                report['note'] = 'Server kept alive for active tasks or Worker drain inspection'
            elif child.poll() is None:
                child.send_signal(signal.SIGTERM)
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    report['server_left_running'] = True
                    report['note'] = 'Server did not stop within 15 seconds'
            if report.get('verdict') != 'PASS':
                report['server_console_tail'] = tail(console_path)
            write_report(path, report)


def main():
    config = None
    report = {'side': 'server', 'phase': 'starting', 'verdict': 'INCOMPLETE',
              'started_at': utc_now()}
    try:
        config = config_and_root()
        path = report_path(config, 'server-report.json')
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
            path = report_path(config, 'server-report.json')
            write_report(path, report)
            print('[SERVER-SMOKE] FAIL: {}; report: {}'.format(error, path), file=sys.stderr)
        else:
            print('[SERVER-SMOKE] FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
