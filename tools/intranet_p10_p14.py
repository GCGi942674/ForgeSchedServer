#!/usr/bin/env python3
"""Three-host P10-P14 acceptance. Run once per host with --role server/worker.

Only child processes launched by this script are signalled. Real Worker
processes are never crash-injected; Worker-loss injection uses a local demo
fixture. This script is compatible with Python 3.6.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from intranet_smoke_common import (ROOT, TERMINAL, config_and_root, number,
                                    path_from, report_path, request, tail,
                                    utc_now, write_report)
from intranet_server_smoke import preflight as server_preflight, wait_for_server
from intranet_worker_smoke import preflight as worker_preflight
from intranet_stage_audit import run_offline
from slot_identity import machine_identity


SCRIPT_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def save_config(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=str(path.parent))
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            for key in sorted(values):
                stream.write('{}={}\n'.format(key, values[key]))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, str(path))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Channel:
    def __init__(self, sock):
        self.sock = sock
        self.reader = sock.makefile('rb')

    def send(self, value):
        raw = json.dumps(value, separators=(',', ':')).encode('utf-8') + b'\n'
        require(len(raw) < 65536, 'control message too large')
        self.sock.sendall(raw)

    def receive(self):
        raw = self.reader.readline(65537)
        require(raw and len(raw) <= 65536 and raw.endswith(b'\n'),
                'control connection closed or invalid frame')
        value = json.loads(raw.decode('utf-8'))
        require(isinstance(value, dict), 'invalid control message')
        return value

    def close(self):
        self.reader.close()
        self.sock.close()


class Peer:
    def __init__(self, channel, hello):
        self.channel = channel
        self.name = hello['machine']
        self.uid = hello['uid']
        self.sequence = 0

    def call(self, op, **kwargs):
        self.sequence += 1
        self.channel.send(dict(op=op, sequence=self.sequence, **kwargs))
        reply = self.channel.receive()
        require(reply.get('sequence') == self.sequence, 'control reply out of order')
        require(reply.get('ok') is True,
                '{} {} failed: {}'.format(self.name, op, reply.get('error')))
        return reply.get('result') or {}


def wait_until(label, seconds, check):
    deadline = time.monotonic() + seconds
    last_error = None
    while time.monotonic() < deadline:
        try:
            result = check()
            if result:
                return result
        except (OSError, ValueError, RuntimeError) as error:
            last_error = str(error)
        time.sleep(1)
    raise RuntimeError('timed out waiting for {}{}'.format(
        label, ': ' + last_error if last_error else ''))


class WorkerAgent:
    def __init__(self, config, token):
        self.config = dict(config)
        self.config['worker.slots'] = '1'
        self.token = token
        self.manager = None
        self.log = None
        self.config_path = None
        self.report = None
        self.report_file = None
        output = path_from(self.config, 'worker.output_root')
        identity = machine_identity(output)
        self.machine = identity['name']
        self.uid = identity['uid']
        hostname = socket.gethostname().split('.')[0]
        override = os.environ.get('GCRP_MACHINE_NAME')
        require(self.machine == hostname or override == self.machine,
                'saved machine identity {} differs from hostname {}; check copied '
                '.machine-identity.json before running'.format(self.machine, hostname))
        worker_preflight(self.config)

    def snapshot(self, count):
        root = path_from(self.config, 'worker.slots_root')
        result = {}
        for index in range(1, count + 1):
            worker_id = '{}-slot{}'.format(self.machine, index)
            checkout = root / worker_id / 'galaxcore'
            svn = checkout / '.svn'
            complete = svn.is_dir() and (checkout / 'test2/run.sh').is_file()
            info = {'complete': complete, 'path': str(checkout)}
            if complete:
                stat = svn.stat()
                info.update(device=stat.st_dev, inode=stat.st_ino)
            result[worker_id] = info
        return result

    def action(self, message):
        op = message.get('op')
        if op == 'START':
            require(self.manager is None, 'manager already started')
            self.config['worker.slots'] = str(message.get('slots'))
            require(self.config['worker.slots'] == '1', 'initial Worker count must be 1')
            save_config(self.config_path, self.config)
            self.log = (self.report_file.parent / 'manager-console.log').open('ab')
            env = dict(os.environ, GCRP_CONFIG=str(self.config_path))
            self.manager = subprocess.Popen([sys.executable, '-B', '-u',
                str(ROOT / 'worker' / 'slot_manager.py')], cwd=str(ROOT), env=env,
                stdout=self.log, stderr=subprocess.STDOUT, close_fds=True)
            time.sleep(1)
            require(self.manager.poll() is None, 'manager exited during startup; inspect log')
            return {'pid': self.manager.pid}
        if op == 'SET_SLOTS':
            require(self.manager is not None and self.manager.poll() is None,
                    'manager is not running')
            slots = message.get('slots')
            require(type(slots) is int and slots in (3, 5), 'only 3 or 5 slots allowed')
            previous = int(self.config['worker.slots'])
            require((previous, slots) in ((1, 3), (3, 5)), 'invalid scale transition')
            self.config['worker.slots'] = str(slots)
            save_config(self.config_path, self.config)
            self.manager.send_signal(signal.SIGHUP)
            return {'slots': slots}
        if op == 'SNAPSHOT':
            count = message.get('slots')
            require(type(count) is int and 1 <= count <= 5, 'invalid snapshot count')
            return {'manager_running': self.manager is not None and
                    self.manager.poll() is None, 'slots': self.snapshot(count)}
        if op == 'STOP':
            require(self.manager is not None, 'manager not started')
            if self.manager.poll() is None:
                self.manager.send_signal(signal.SIGTERM)
                self.manager.wait(timeout=180)
            require(self.manager.returncode == 0, 'manager stopped with {}'.format(
                    self.manager.returncode))
            return {'exit_code': self.manager.returncode}
        if op == 'EXIT':
            return {'exit': True}
        raise ValueError('unsupported control action')

    def run(self):
        port = number(self.config, 'smoke.control_port',
                      int(self.config['server.port']) + 1, 1, 65535)
        host = self.config.get('network.server_ip')
        require(host, 'configure network.server_ip')
        sock = wait_until('acceptance controller', 1200,
                          lambda: socket.create_connection((host, port), timeout=3))
        sock.settimeout(240)
        channel = Channel(sock)
        try:
            channel.send({'token': self.token, 'machine': self.machine,
                          'uid': self.uid, 'hostname': socket.gethostname(),
                          'script_sha256': SCRIPT_SHA256})
            welcome = channel.receive()
            require(welcome.get('ok') is True, 'controller rejected Worker: ' +
                    str(welcome.get('error')))
            # Checkout and real PJtest runs may exceed the socket's handshake
            # timeout; the controller owns the bounded stage deadlines.
            sock.settimeout(None)
            run_id = welcome['run_id']
            directory = report_path(self.config, 'p10-p14-' + run_id)
            directory.mkdir(parents=True, exist_ok=True)
            self.config_path = directory / 'worker.conf'
            self.report_file = directory / 'worker-report.json'
            self.report = {'role': 'worker', 'machine': self.machine,
                           'run_id': run_id, 'started_at': utc_now(),
                           'verdict': 'INCOMPLETE', 'actions': []}
            write_report(self.report_file, self.report)
            print('[ACCEPTANCE] {} connected; report {}'.format(
                self.machine, self.report_file), flush=True)
            while True:
                message = channel.receive()
                sequence = message.get('sequence')
                try:
                    result = self.action(message)
                    reply = {'sequence': sequence, 'ok': True, 'result': result}
                    self.report['actions'].append({'op': message.get('op'),
                                                   'result': result})
                except (OSError, ValueError, RuntimeError,
                        subprocess.TimeoutExpired) as error:
                    reply = {'sequence': sequence, 'ok': False, 'error': str(error)}
                    self.report['verdict'] = 'FAIL'
                    self.report['error'] = str(error)
                write_report(self.report_file, self.report)
                channel.send(reply)
                if message.get('op') == 'EXIT' and reply['ok']:
                    self.report['verdict'] = 'PASS'
                    self.report['finished_at'] = utc_now()
                    write_report(self.report_file, self.report)
                    break
        finally:
            if self.report is not None and self.report['verdict'] == 'INCOMPLETE':
                self.report['note'] = 'control ended; any active manager was left running'
                write_report(self.report_file, self.report)
            channel.close()
            if self.log is not None:
                self.log.close()


class ServerHarness:
    def __init__(self, config, directory):
        self.config = dict(config)
        self.directory = directory
        self.config_path = directory / 'server.conf'
        self.log_path = directory / 'server-console.log'
        self.database = directory / 'state.db'
        self.config['database.path'] = str(self.database)
        self.config['smoke.report_dir'] = str(directory)
        self.binary = server_preflight(self.config)['binary']
        self.process = None
        self.log = None

    def start(self):
        require(self.process is None or self.process.poll() is not None,
                'Server already running')
        save_config(self.config_path, self.config)
        self.log = self.log_path.open('ab')
        env = dict(os.environ, GCRP_CONFIG=str(self.config_path))
        self.process = subprocess.Popen([self.binary], cwd=str(ROOT), env=env,
                                        stdout=self.log, stderr=subprocess.STDOUT,
                                        close_fds=True)
        wait_for_server(self.config, self.process, 30)
        print('[ACCEPTANCE] Server ready pid={}'.format(self.process.pid), flush=True)

    def stop(self, crash=False):
        require(self.process is not None, 'Server never started')
        if self.process.poll() is None:
            if crash:
                self.process.kill()
            else:
                self.process.send_signal(signal.SIGTERM)
            self.process.wait(timeout=30)
        if self.log is not None:
            self.log.close()
            self.log = None

    def rpc(self, kind, data):
        require(self.process is not None and self.process.poll() is None,
                'Server exited during test')
        return request(self.config, kind, data)

    def task(self, task_id):
        return self.rpc('query_task', {'task_id': task_id})

    def online(self):
        status = self.rpc('query_status', {})
        return {item['worker_id']: item for item in status.get('workers', [])
                if item.get('status') == 'ONLINE'}


def task_body(setup, batch, index, timeout):
    return {'task_type': 'REGRESSION', 'target': setup['target'],
            'revision': setup['revision'], 'priority': 0,
            'submission_key': '{}-{}'.format(batch, index),
            'payload': {'spec_version': 2, 'case': setup['case'],
                        'flow': setup['flow'], 'timeout_seconds': timeout,
                        'flow_config': setup['flow_config'],
                        'context': {'suite': 'p10_p14_acceptance',
                                    'batch_id': batch, 'template': setup['flow']}}}


def wait_tasks(harness, ids, seconds):
    def complete():
        items = [harness.task(task_id) for task_id in ids]
        return items if all(item['status'] in TERMINAL for item in items) else None
    return wait_until('task results', seconds, complete)


def paginate(harness, batch):
    query = {'batch_id': batch, 'limit': 1}
    items, counts = [], None
    for _ in range(100):
        page = harness.rpc('query_regression', query)
        if counts is None:
            counts = page['counts']
        require(page['counts'] == counts, 'regression counts changed between pages')
        items.extend(page['items'])
        if not page['has_more']:
            require(len(items) == page['total'], 'regression total/page mismatch')
            return {'total': page['total'], 'counts': counts,
                    'task_ids': [item['task_id'] for item in items]}
        cursor = page.get('next_before_task_id')
        require(type(cursor) is int and cursor > 0, 'invalid regression cursor')
        query['before_task_id'] = cursor
    raise RuntimeError('regression query exceeded 100 pages')


def server_main(config, args):
    require(args.scale_host, 'specify --scale-host (novarch3 or novarch4)')
    require(args.token is None, 'Server generates its own one-time control token')
    run_id = uuid.uuid4().hex[:12]
    token = uuid.uuid4().hex + uuid.uuid4().hex
    directory = report_path(config, 'p10-p14-' + run_id)
    directory.mkdir(parents=True, exist_ok=False)
    report_file = directory / 'server-report.json'
    report = {'role': 'server', 'run_id': run_id, 'verdict': 'INCOMPLETE',
              'started_at': utc_now(), 'stages': {}}
    write_report(report_file, report)
    harness = None
    peers = {}
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        bind_ip = config.get('server.bind_ip') or '127.0.0.1'
        port = number(config, 'server.port', 0, 1, 65534)
        control_port = number(config, 'smoke.control_port', port + 1, 1, 65535)
        require(control_port != port, 'control port must differ from Server port')
        # Refuse to touch any already-running Server on this port.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind((bind_ip, port))
        listener.bind((bind_ip, control_port))
        listener.listen(2)
        listener.settimeout(1200)
        setup = server_preflight(config)
        require(not (directory / 'state.db').exists(), 'test database already exists')
        print('[ACCEPTANCE] run={} control_port={}'.format(run_id, control_port), flush=True)
        print('[ACCEPTANCE] On novarch3 and novarch4 run: python3 -B -u '
              'tools/intranet_p10_p14.py --role worker --token {}'.format(token), flush=True)
        while len(peers) < 2:
            sock, _address = listener.accept()
            sock.settimeout(240)
            channel = Channel(sock)
            try:
                hello = channel.receive()
                name, uid = hello.get('machine'), hello.get('uid')
                require(hello.get('token') == token and isinstance(name, str)
                        and isinstance(uid, str), 'invalid Worker control handshake')
                require(hello.get('script_sha256') == SCRIPT_SHA256,
                        'controller and Worker acceptance scripts differ')
                require(name not in peers and uid not in [p.uid for p in peers.values()],
                        'duplicate Worker identity; check copied worker-output')
                require(name in ('novarch3', 'novarch4'),
                        'expected novarch3/novarch4, got ' + name)
                channel.send({'ok': True, 'run_id': run_id})
                peers[name] = Peer(channel, hello)
                print('[ACCEPTANCE] agent connected: ' + name, flush=True)
            except (OSError, ValueError, RuntimeError):
                channel.close()
                raise
        require(args.scale_host in peers, 'scale host is not connected')
        other = next(name for name in peers if name != args.scale_host)
        scale = peers[args.scale_host]
        harness = ServerHarness(config, directory)
        report.update(revision=setup['revision'], case=setup['case'],
                      database=str(harness.database), submitted_task_ids=[])
        write_report(report_file, report)
        harness.start()
        require(harness.rpc('query_status', {}).get('total') == 0,
                'isolated test database is not empty')
        scale.call('START', slots=1)
        wait_until('one online Worker', 1200,
                   lambda: len(harness.online()) == 1 and
                   args.scale_host + '-slot1' in harness.online())
        timeout = number(config, 'smoke.case_timeout_seconds', 5400, 1, 86400)
        wait_seconds = number(config, 'smoke.wait_seconds', 12000, 30, 172800)
        batch10 = 'p10-' + run_id
        ids10 = [harness.rpc('submit_task', task_body(setup, batch10, i, timeout))
                 ['task_id'] for i in range(1, 4)]
        report['submitted_task_ids'].extend(ids10)
        report['phase'] = 'P10_running'
        write_report(report_file, report)
        def one_running_two_queued():
            tasks = [harness.task(task_id) for task_id in ids10]
            return tasks if sorted(item['status'] for item in tasks) == [
                'QUEUED', 'QUEUED', 'RUNNING'] else None
        wait_until('one RUNNING and two QUEUED cases', 60, one_running_two_queued)
        cancel = harness.rpc('cancel_batch', {'batch_id': batch10})
        require(cancel['cancelled_now'] == 2 and cancel['in_flight'] == 1,
                'unexpected batch cancellation response')
        tasks10 = wait_tasks(harness, ids10, wait_seconds)
        require(sum(item['status'] == 'SUCCEEDED' and
                    (item.get('execution') or {}).get('case_status') == 'PASS'
                    for item in tasks10) == 1 and
                sum(item['status'] == 'CANCELLED' for item in tasks10) == 2,
                'P10 expected one real PASS and two queued CANCELLED')
        require(harness.rpc('query_batch', {'batch_id': batch10})['complete'],
                'P10 batch did not complete')
        report['stages']['P10'] = {'status': 'PASS', 'task_ids': ids10,
                                   'cancelled_now': cancel['cancelled_now']}
        write_report(report_file, report)
        print('[ACCEPTANCE] P10 PASS', flush=True)

        # P11: restart gracefully with the *same* private SQLite database.
        before = {item['task_id']: item for item in tasks10}
        harness.stop()
        with sqlite3.connect('file:{}?mode=ro'.format(harness.database.as_posix()),
                             uri=True, timeout=5) as db:
            require(db.execute('PRAGMA quick_check').fetchone()[0] == 'ok',
                    'SQLite quick_check failed')
        harness.start()
        after = {task_id: harness.task(task_id) for task_id in ids10}
        require(all(after[key]['status'] == before[key]['status'] and
                    after[key].get('run_id') == before[key].get('run_id')
                    for key in before), 'P11 task state changed across restart')
        require(harness.rpc('query_batch', {'batch_id': batch10})['complete'],
                'P11 batch state was not restored')
        report['stages']['P11'] = {'status': 'PASS', 'database': str(harness.database),
                                   'task_ids': ids10}
        write_report(report_file, report)
        print('[ACCEPTANCE] P11 PASS', flush=True)

        # P12: only the Server is crash-injected. The real Worker keeps running.
        batch12 = 'p12-' + run_id
        id12 = harness.rpc('submit_task', task_body(setup, batch12, 1, timeout))['task_id']
        report['submitted_task_ids'].append(id12)
        report['phase'] = 'P12_running'
        write_report(report_file, report)
        require(id12 not in ids10, 'P11 reused a task ID after restart')
        running = wait_until('P12 RUNNING task', 60,
                             lambda: harness.task(id12) if
                             harness.task(id12)['status'] == 'RUNNING' else None)
        old_run = running.get('run_id')
        require(old_run, 'P12 RUNNING task has no run_id')
        harness.stop(crash=True)
        harness.start()
        task12 = wait_tasks(harness, [id12], wait_seconds)[0]
        require(task12['status'] == 'SUCCEEDED' and
                (task12.get('execution') or {}).get('case_status') == 'PASS' and
                task12.get('run_id') == old_run and task12.get('retry_count') == 0,
                'P12 real case was duplicated or not recovered')
        report['stages']['P12'] = {'status': 'PASS', 'task_id': id12,
                                   'run_id': old_run, 'retry_count': 0,
                                   'worker_crash': 'checked by offline demo fixture'}
        write_report(report_file, report)
        print('[ACCEPTANCE] P12 Server restart PASS', flush=True)

        # P13: scale the selected host 1 -> 3 -> 5, keep the second host at 1.
        peers[other].call('START', slots=1)
        wait_until('two online Workers', 1200,
                   lambda: len(harness.online()) == 2)
        scale.call('SET_SLOTS', slots=3)
        expected3 = {args.scale_host + '-slot{}'.format(i) for i in range(1, 4)}
        expected3.add(other + '-slot1')
        wait_until('three slots plus second host', 1200,
                   lambda: set(harness.online()) == expected3)
        snapshot3 = scale.call('SNAPSHOT', slots=3)['slots']
        require(all(item['complete'] for item in snapshot3.values()),
                'P13 incomplete SVN copy at three slots')
        scale.call('SET_SLOTS', slots=5)
        expected5 = {args.scale_host + '-slot{}'.format(i) for i in range(1, 6)}
        expected5.add(other + '-slot1')
        wait_until('five slots plus second host', 1200,
                   lambda: set(harness.online()) == expected5)
        snapshot5 = scale.call('SNAPSHOT', slots=5)['slots']
        require(all(item['complete'] for item in snapshot5.values()),
                'P13 incomplete SVN copy at five slots')
        require(all(snapshot5[key]['device'] == value['device'] and
                    snapshot5[key]['inode'] == value['inode']
                    for key, value in snapshot3.items()),
                'P13 existing slot checkout was replaced during expansion')
        report['stages']['P13'] = {'status': 'PASS', 'scale_host': args.scale_host,
                                   'worker_ids': sorted(expected5),
                                   'note': 'registration and SVN copies; two-host real execution '
                                           'was verified separately'}
        write_report(report_file, report)
        print('[ACCEPTANCE] P13 3->5 PASS', flush=True)

        # P14: query actual P10/P12 batches with one-item pages.
        regression10 = paginate(harness, batch10)
        regression12 = paginate(harness, batch12)
        require(set(regression10['task_ids']) == set(ids10) and
                regression10['counts'].get('PASS') == 1 and
                regression10['counts'].get('CANCELLED') == 2,
                'P14 P10 batch outcomes do not match')
        require(regression12['task_ids'] == [id12] and
                regression12['counts'].get('PASS') == 1,
                'P14 P12 batch outcome does not match')
        report['stages']['P14'] = {'status': 'PASS', 'P10': regression10,
                                   'P12': regression12}
        write_report(report_file, report)
        print('[ACCEPTANCE] P14 PASS', flush=True)

        # Also require all P10-P14 offline fixtures, including demo Worker loss.
        audit = {'offline_tests': {}}
        run_offline(harness.config, audit)
        report['offline_tests'] = audit.get('offline_tests', {})
        report['offline_error'] = audit.get('offline_error')
        write_report(report_file, report)
        require(not audit.get('offline_error') and all(
            item.get('status') == 'PASS' for group in audit['offline_tests'].values()
            for item in group.values()), 'offline P10-P14 tests failed; inspect stage-test-logs')
        report['verdict'] = 'PASS'
        report['finished_at'] = utc_now()
        write_report(report_file, report)
        print('[ACCEPTANCE] P10-P14 PASS; report: ' + str(report_file), flush=True)
        for peer in peers.values():
            peer.call('STOP')
            peer.call('EXIT')
        harness.stop()
        return 0
    except (OSError, ValueError, RuntimeError, sqlite3.Error,
            subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        report.update(verdict='FAIL', error=str(error), finished_at=utc_now())
        if harness is not None:
            report['server_pid'] = harness.process.pid if harness.process else None
            report['server_running'] = bool(harness.process and
                                            harness.process.poll() is None)
            report['server_console_tail'] = tail(harness.log_path)
        report['note'] = ('Processes with active real cases are left for inspection; '
                          'do not delete the DB, journals or slot directories')
        write_report(report_file, report)
        print('[ACCEPTANCE] FAIL: {}; report: {}'.format(error, report_file),
              file=sys.stderr, flush=True)
        return 1
    finally:
        listener.close()
        for peer in peers.values():
            try:
                peer.channel.close()
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('server', 'worker'), required=True)
    parser.add_argument('--scale-host', choices=('novarch3', 'novarch4'))
    parser.add_argument('--token', help='one-time token printed by Server controller')
    args = parser.parse_args()
    try:
        config = config_and_root()
        if args.role == 'server':
            return server_main(config, args)
        require(args.token, 'Worker requires --token from Server controller')
        agent = WorkerAgent(config, args.token)
        agent.run()
        return 0
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        print('[ACCEPTANCE] {}: {}'.format(args.role, error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
