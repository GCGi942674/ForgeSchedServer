#!/usr/bin/env python3
"""Run both production smoke launchers against a disposable SVN/PJTest fixture."""
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
    server_binary = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='forgesched-intranet-') as temporary:
        base = Path(temporary)
        subprocess.run([sys.executable, '-B', str(root / 'tests/fixtures/pjtest/setup.py'),
                        str(base)], check=True, stdout=subprocess.DEVNULL)
        # Production ZIPs may have no publisher manifest. The shared fixture
        # manifest hashes a mutable test2 tree and cannot survive a second run.
        (base / 'artifacts/GalaxCore_58231.zip.manifest.json').unlink()
        svn_repo = base / 'svn-repository'
        subprocess.run(['svnadmin', 'create', str(svn_repo)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(['svn', 'import', '--non-interactive', '-m', 'fixture',
                        str(base / 'slot'), svn_repo.as_uri()], check=True,
                       stdout=subprocess.DEVNULL)
        template = base / 'templates'
        template.mkdir()
        (template / 'route.json').write_text(json.dumps(
            {'flow_config': {'route_design': 1, 'original': 1}}), encoding='utf-8')
        port = free_port()
        config = base / 'config.conf'
        settings = {
            'server.bind_ip': '127.0.0.1', 'network.server_ip': '127.0.0.1',
            'server.port': str(port), 'server.io_threads': '2',
            'server.worker_threads': '2', 'network.timeout_ms': '2000',
            'log.level': 'ERROR', 'log.dir': str(base / 'logs'),
            'database.path': str(base / 'state.db'),
            'server.work_root': str(base / 'slot/test2'),
            'server.artifact_root': str(base / 'artifacts'),
            'server.templates_dir': str(template),
            'tools.server_binary': str(server_binary),
            'worker.machine_id': 'fixture', 'worker.slot_ids': 'slot-1',
            'worker.slots_root': str(base / 'worker-slots'),
            'worker.output_root': str(base / 'output'),
            'worker.svn_url': svn_repo.as_uri(),
            'worker.artifact_root': str(base / 'artifacts'),
            'worker.checkout_parallelism': '1',
            'worker.terminate_grace_seconds': '1', 'worker.reconnect_seconds': '1',
            'smoke.revision': '58231', 'smoke.case': 'cases/success/run.tcl',
            'smoke.flow': 'route', 'smoke.target': 'fixture', 'smoke.run_count': '2',
            'smoke.case_timeout_seconds': '30',
            'smoke.expected_worker_id': 'fixture-slot-1',
            'smoke.report_dir': str(base / 'reports'),
            'smoke.connect_wait_seconds': '60', 'smoke.wait_seconds': '120',
        }
        config.write_text(''.join('{}={}\n'.format(key, value)
                                  for key, value in settings.items()), encoding='utf-8')
        env = os.environ.copy()
        env['FORGESCHED_CONFIG'] = str(config)
        server = worker = None
        try:
            with (base / 'server-script.log').open('w+') as server_output, \
                    (base / 'worker-script.log').open('w+') as worker_output:
                server = subprocess.Popen([sys.executable, '-B', '-u',
                    str(root / 'tools/intranet_server_smoke.py')],
                    cwd=str(root), env=env, stdout=server_output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                time.sleep(1)
                worker = subprocess.Popen([sys.executable, '-B', '-u',
                    str(root / 'tools/intranet_worker_smoke.py')],
                    cwd=str(root), env=env, stdout=worker_output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                server_code = server.wait(timeout=100)
                worker_code = worker.wait(timeout=20)
                if server_code != 0 or worker_code != 0:
                    server_output.seek(0)
                    worker_output.seek(0)
                    raise AssertionError('server={} worker={}\nserver script:\n{}\nworker script:\n{}\nserver report:\n{}\nworker report:\n{}'.format(
                        server_code, worker_code, server_output.read()[-4000:],
                        worker_output.read()[-4000:],
                        (base / 'reports/server-report.json').read_text() if
                        (base / 'reports/server-report.json').is_file() else 'missing',
                        (base / 'reports/worker-report.json').read_text() if
                        (base / 'reports/worker-report.json').is_file() else 'missing'))
            server_report = json.loads((base / 'reports/server-report.json').read_text())
            worker_report = json.loads((base / 'reports/worker-report.json').read_text())
            assert server_report['verdict'] == worker_report['verdict'] == 'PASS'
            assert server_report['worker_drained']
            assert len(server_report['tasks']) == len(worker_report['task_results']) == 2
            assert all(item['execution']['case_status'] == 'PASS'
                       for item in server_report['tasks'])
            assert (base / 'worker-slots/fixture-slot-1/galaxcore/.svn').is_dir()
        finally:
            stop_group(server)
            stop_group(worker)


if __name__ == '__main__':
    main()
