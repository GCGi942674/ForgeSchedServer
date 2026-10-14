#!/usr/bin/env python3
"""Simulate two separate one-slot machines against a disposable SVN fixture."""
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

def write_config(path, settings):
    path.write_text(''.join('{}={}\n'.format(key, value)
                            for key, value in settings.items()), encoding='utf-8')

def main():
    binary = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='gcrp-two-machines-') as temporary:
        base = Path(temporary)
        subprocess.run([sys.executable, '-B', str(root / 'tests/fixtures/pjtest/setup.py'),
                        str(base)], check=True, stdout=subprocess.DEVNULL)
        (base / 'artifacts/GalaxCore_58231.zip.manifest.json').unlink()
        runner = base / 'slot/test2/run.sh'
        runner.write_text(runner.read_text().replace(
            'if [ "$mode" = timeout ]; then',
            'if [ "$mode" = success ]; then sleep 2; fi\n'
            'if [ "$mode" = timeout ]; then'))
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
        common = {
            'network.server_ip': '127.0.0.1', 'server.port': str(free_port()),
            'network.timeout_ms': '2000',
            'worker.svn_url': svn_repo.as_uri(),
            'worker.artifact_root': str(base / 'artifacts'),
            'worker.checkout_parallelism': '1',
            'worker.reconnect_seconds': '1',
            'worker.terminate_grace_seconds': '1',
            'smoke.revision': '58231',
            'smoke.connect_wait_seconds': '60', 'smoke.wait_seconds': '120',
        }
        server_settings = dict(common)
        server_settings.update({
            'server.bind_ip': '127.0.0.1',
            'server.io_threads': '2', 'server.worker_threads': '2',
            'log.level': 'ERROR', 'log.dir': str(base / 'logs'),
            'database.path': str(base / 'state.db'),
            'server.work_root': str(base / 'slot/test2'),
            'server.artifact_root': str(base / 'artifacts'),
            'server.templates_dir': str(templates),
            'tools.server_binary': str(binary),
            'smoke.case': 'cases/success/run.tcl', 'smoke.flow': 'route',
            'smoke.target': 'fixture', 'smoke.run_count': '2',
            'smoke.case_timeout_seconds': '30',
            'smoke.expected_worker_ids': '',
            'smoke.require_distinct_machines': '1',
            'smoke.report_dir': str(base / 'server-reports'),
        })
        server_config = base / 'server.conf'
        write_config(server_config, server_settings)
        worker_configs = []
        for machine in ('fixture-a', 'fixture-b'):
            settings = dict(common)
            settings.update({
                'worker.slots': '1',
                'worker.slots_root': str(base / machine / 'slots'),
                'worker.output_root': str(base / machine / 'output'),
                'smoke.report_dir': str(base / machine / 'reports'),
            })
            config = base / (machine + '.conf')
            write_config(config, settings)
            worker_configs.append(config)
        processes = []
        try:
            def start(script, config, log):
                env = os.environ.copy()
                env['GCRP_CONFIG'] = str(config)
                if config in worker_configs:
                    env['GCRP_MACHINE_NAME'] = config.stem
                stream = log.open('w')
                try:
                    process = subprocess.Popen([sys.executable, '-B', '-u', str(script)],
                        cwd=str(root), env=env, stdout=stream,
                        stderr=subprocess.STDOUT, start_new_session=True)
                finally:
                    stream.close()
                processes.append(process)
                return process
            server = start(root / 'tools/intranet_dual_server_smoke.py',
                           server_config, base / 'server-script.log')
            time.sleep(1)
            workers = [start(root / 'tools/intranet_two_machine_worker_smoke.py',
                             config, base / (config.stem + '-script.log'))
                       for config in worker_configs]
            codes = [server.wait(timeout=100)] + [p.wait(timeout=20) for p in workers]
            if codes != [0, 0, 0]:
                raise AssertionError('exit codes {}\nserver {}\nworker-a {}\nworker-b {}'
                    .format(codes, (base / 'server-script.log').read_text()[-3000:],
                        (base / 'fixture-a-script.log').read_text()[-3000:],
                        (base / 'fixture-b-script.log').read_text()[-3000:]))
            server_report = json.loads((base / 'server-reports/dual-server-report.json').read_text())
            reports = [json.loads((base / machine / 'reports/two-machine-worker-report.json').read_text())
                       for machine in ('fixture-a', 'fixture-b')]
            assert server_report['verdict'] == 'PASS'
            assert server_report['workers_drained']
            assert server_report['overlap_ms'] > 0
            assert set(server_report['machine_ids'].values()) == {'fixture-a', 'fixture-b'}
            assert all(report['verdict'] == 'PASS' and report['checkout_complete']
                       and len(report['task_results']) == 1 for report in reports)
            assert {report['task_results'][0]['worker_id'] for report in reports} == {
                'fixture-a-slot1', 'fixture-b-slot1'}
        finally:
            for process in processes:
                stop_group(process)

if __name__ == '__main__':
    main()
