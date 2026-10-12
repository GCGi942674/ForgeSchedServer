#!/usr/bin/env python3
"""Run dual-slot launchers against a disposable local SVN/PJTest fixture."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

def stop_group(process):
    if process is None or process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=10)

def main():
    binary = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='forgesched-dual-') as temporary:
        base = Path(temporary)
        subprocess.run([sys.executable, '-B', str(root / 'tests/fixtures/pjtest/setup.py'),
                        str(base)], check=True, stdout=subprocess.DEVNULL)
        (base / 'artifacts/GalaxCore_58231.zip.manifest.json').unlink()
        runner = base / 'slot/test2/run.sh'
        source = runner.read_text()
        source = source.replace('if [ "$mode" = timeout ]; then',
                                'if [ "$mode" = success ]; then sleep 2; fi\n'
                                'if [ "$mode" = timeout ]; then')
        runner.write_text(source)
        svn_repo = base / 'svn-repository'
        subprocess.run(['svnadmin', 'create', str(svn_repo)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(['svn', 'import', '--non-interactive', '-m', 'fixture',
                        str(base / 'slot'), svn_repo.as_uri()], check=True,
                       stdout=subprocess.DEVNULL)
        templates = base / 'templates'
        templates.mkdir()
        (templates / 'route.json').write_text(json.dumps(
            {'flow_config': {'route_design': 1, 'original': 1}}))
        settings = {
            'server.bind_ip': '127.0.0.1', 'network.server_ip': '127.0.0.1',
            'server.port': str(free_port()), 'server.io_threads': '2',
            'server.worker_threads': '2', 'network.timeout_ms': '2000',
            'log.level': 'ERROR', 'log.dir': str(base / 'logs'),
            'database.path': str(base / 'state.db'),
            'server.work_root': str(base / 'slot/test2'),
            'server.artifact_root': str(base / 'artifacts'),
            'server.templates_dir': str(templates),
            'tools.server_binary': str(binary),
            'worker.slots': '2',
            'worker.slots_root': str(base / 'worker-slots'),
            'worker.output_root': str(base / 'output'),
            'worker.svn_url': svn_repo.as_uri(),
            'worker.artifact_root': str(base / 'artifacts'),
            'worker.checkout_parallelism': '1',
            'worker.terminate_grace_seconds': '1', 'worker.reconnect_seconds': '1',
            'smoke.revision': '58231', 'smoke.case': 'cases/success/run.tcl',
            'smoke.flow': 'route', 'smoke.target': 'fixture',
            'smoke.run_count': '2', 'smoke.case_timeout_seconds': '30',
            'smoke.expected_worker_ids': '',
            'smoke.report_dir': str(base / 'reports'),
            'smoke.connect_wait_seconds': '60', 'smoke.wait_seconds': '120',
        }
        config = base / 'config.conf'
        config.write_text(''.join('{}={}\n'.format(key, value)
                                  for key, value in settings.items()))
        env = os.environ.copy()
        env['FORGESCHED_CONFIG'] = str(config)
        env['FORGESCHED_MACHINE_NAME'] = 'fixture'
        server = worker = None
        try:
            with (base / 'server-script.log').open('w+') as server_output, \
                    (base / 'worker-script.log').open('w+') as worker_output:
                server = subprocess.Popen([sys.executable, '-B', '-u',
                    str(root / 'tools/intranet_dual_server_smoke.py')],
                    cwd=str(root), env=env, stdout=server_output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                time.sleep(1)
                worker = subprocess.Popen([sys.executable, '-B', '-u',
                    str(root / 'tools/intranet_dual_worker_smoke.py')],
                    cwd=str(root), env=env, stdout=worker_output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                server_code = server.wait(timeout=100)
                worker_code = worker.wait(timeout=20)
                if server_code != 0 or worker_code != 0:
                    server_output.seek(0)
                    worker_output.seek(0)
                    raise AssertionError('server={} worker={}\n{}\n{}\n{}\n{}'.format(
                        server_code, worker_code, server_output.read()[-4000:],
                        worker_output.read()[-4000:],
                        (base / 'reports/dual-server-report.json').read_text(),
                        (base / 'reports/dual-worker-report.json').read_text()))
            server_report = json.loads((base / 'reports/dual-server-report.json').read_text())
            worker_report = json.loads((base / 'reports/dual-worker-report.json').read_text())
            assert server_report['verdict'] == worker_report['verdict'] == 'PASS'
            assert server_report['workers_drained']
            assert server_report['overlap_ms'] > 0
            assert {item['worker_id'] for item in server_report['tasks']} == {
                'fixture-slot1', 'fixture-slot2'}
            assert all(worker_report['checkout_complete'].values())
            assert len(worker_report['task_results']) == 2
        finally:
            stop_group(server)
            stop_group(worker)

if __name__ == '__main__':
    main()
