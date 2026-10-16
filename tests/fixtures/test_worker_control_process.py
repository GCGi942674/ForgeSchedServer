"""Real TCP, Worker and local SVN update fixture. No GalaxCore installation required."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from test_sqlite_worker import request, wait_server, stop


def eventually(check, seconds=35):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        last = check()
        if last:
            return last
        time.sleep(.05)
    raise AssertionError('condition timed out: ' + repr(last))


def main():
    server_binary, project = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='gcrp-management-') as temporary:
        base = Path(temporary)
        subprocess.check_call([sys.executable, '-B', str(project / 'tests/fixtures/pjtest/setup.py'), str(base)], stdout=subprocess.DEVNULL)
        (base / 'artifacts/GalaxCore_58231.zip.manifest.json').unlink()
        script = base / 'slot/test2/run.sh'
        script.write_text(script.read_text().replace('case_dir=$(dirname', 'sleep 4\ncase_dir=$(dirname'))
        repository = base / 'repository'
        def svn(*args):
            subprocess.run(['svn', '--non-interactive', *map(str, args)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.check_call(['svnadmin', 'create', str(repository)])
        svn('import', '-m', 'fixture', base / 'slot', repository.as_uri())
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        config = base / 'settings.conf'
        template = base / 'templates'
        template.mkdir()
        (template / 'route.json').write_text(json.dumps({'flow_config': {'route_design': 1, 'original': 1}}))
        config.write_text('server.bind_ip=127.0.0.1\nserver.port={}\nnetwork.server_ip=127.0.0.1\n'
            'server.io_threads=1\nserver.worker_threads=2\nlog.level=ERROR\nlog.dir={}\n'
            'database.path={}\nserver.templates_dir={}\nserver.log_archive_root={}\n'
            'worker.slots_root={}\nworker.svn_url={}\nworker.artifact_root={}\nworker.output_dir={}\n'
            'worker.reconnect_seconds=0.1\nworker.terminate_grace_seconds=0.1\n'.format(
                port, base / 'logs', base / 'state.db', template, base / 'archive', base / 'slots',
                repository.as_uri(), base / 'artifacts', base / 'output'))
        environment = dict(os.environ, GCRP_CONFIG=str(config))
        server = worker = None
        with (base / 'server.log').open('w+') as server_log, (base / 'worker.log').open('w+') as worker_log:
            def start_server():
                process = subprocess.Popen([str(server_binary)], env=environment, cwd=str(base),
                                           stdout=server_log, stderr=subprocess.STDOUT)
                wait_server(port, process)
                return process
            def call(kind, data):
                reply = request(port, kind, data)
                assert reply['code'] == 0, reply
                return reply['result']
            def worker_row():
                rows = call('query_workers', {})['items']
                return next((row for row in rows if row['worker_id'] == 'fixture-slot1'), None)
            def control(action, key, **extra):
                return call('control_workers', dict(machine_id='fixture', action=action, operation_id=key, **extra))
            def submit():
                return call('submit_task', dict(task_type='REGRESSION', target='fixture', revision='58231',
                    payload=dict(spec_version=2, case='cases/success/run.tcl', flow='route', timeout_seconds=30,
                                 flow_config={'route_design': 1, 'original': 1})))['task_id']
            def task(identity):
                return call('query_task', dict(task_id=identity))
            try:
                server = start_server()
                worker = subprocess.Popen([sys.executable, '-B', '-u', str(project / 'worker/gcrp_worker.py'),
                    '--worker-id', 'fixture-slot1', '--machine-id', 'fixture', '--slot-id', 'slot1'],
                    env=environment, cwd=str(base), stdout=worker_log, stderr=subprocess.STDOUT)
                eventually(lambda: worker_row() and worker_row()['control_supported'])
                bad = request(port, 'control_workers', dict(machine_id='fixture', action='update',
                                                           operation_id='bad', scope='../other'))
                assert bad['code'] == 1, bad
                first = submit()
                eventually(lambda: task(first)['status'] == 'RUNNING')
                second = submit()
                paused = control('pause', 'pause-active')
                assert paused['accepted'] == 1
                assert task(first)['status'] == 'RUNNING'
                eventually(lambda: task(first)['status'] == 'SUCCEEDED')
                assert task(second)['status'] == 'QUEUED'
                marker = base / 'marker.txt'
                marker.write_text('repository-v2\n')
                svn('import', '-m', 'new marker', marker, repository.as_uri() + '/test2/update_marker.txt')
                accepted = control('update', 'update-once', scope='test2')
                eventually(lambda: worker_row()['maintenance']['state'] == 'SUCCEEDED')
                checkout = base / 'slots/fixture-slot1/galaxcore'
                assert (checkout / 'test2/update_marker.txt').read_text() == 'repository-v2\n'
                assert worker_row()['accepting_tasks'] is False
                assert control('update', 'update-once', scope='test2') == accepted
                journal = json.loads((base / 'output/maintenance.json').read_text())
                assert journal['operation_id'] == 'update-once'
                assert (base / 'output/maintenance-log-update-once.log').read_text().count('[MAINTENANCE]') == 1
                stop(server)
                server = start_server()
                eventually(lambda: worker_row() and worker_row()['status'] == 'ONLINE')
                assert worker_row()['accepting_tasks'] is False
                assert task(second)['status'] == 'QUEUED'
                assert control('update', 'update-once', scope='test2') == accepted
                control('resume', 'resume-after-restart')
                eventually(lambda: task(second)['status'] == 'SUCCEEDED')
                # Create an actual SVN text conflict; no revert/delete is allowed.
                writer = base / 'writer'
                svn('checkout', repository.as_uri(), writer)
                (writer / 'test2/update_marker.txt').write_text('repository-v3\n')
                svn('commit', '-m', 'remote edit', writer / 'test2/update_marker.txt')
                (checkout / 'test2/update_marker.txt').write_text('local edit\n')
                control('update', 'update-conflict', scope='all')
                eventually(lambda: worker_row()['maintenance']['state'] == 'FAILED')
                assert worker_row()['accepting_tasks'] is False
                assert 'local edit' in (checkout / 'test2/update_marker.txt').read_text()
                third = submit()
                time.sleep(1)
                assert task(third)['status'] == 'QUEUED'
                assert worker.poll() is None
                print('WORKER_CONTROL_PROCESS PASS: pause, no active kill, SVN update, replay, restart, resume, conflict quarantine')
            except BaseException:
                try:
                    print('WORKER STATE: ' + repr(worker_row()), file=sys.stderr)
                except Exception:
                    pass
                for name, stream in (('SERVER', server_log), ('WORKER', worker_log)):
                    stream.flush()
                    stream.seek(0)
                    print(name + ':\n' + stream.read()[-6000:], file=sys.stderr)
                raise
            finally:
                stop(worker)
                stop(server)


if __name__ == '__main__':
    main()
