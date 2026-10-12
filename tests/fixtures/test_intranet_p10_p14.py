#!/usr/bin/env python3
"""End-to-end dry environment for the three-host P10-P14 controller."""
import json
import os
from pathlib import Path
import re
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


def write_config(path, settings):
    path.write_text(''.join('{}={}\n'.format(key, value)
                            for key, value in settings.items()), encoding='utf-8')


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
    with tempfile.TemporaryDirectory(prefix='forge-p10-p14-') as temporary:
        base = Path(temporary)
        subprocess.run([sys.executable, '-B', str(root / 'tests/fixtures/pjtest/setup.py'),
                        str(base)], check=True, stdout=subprocess.DEVNULL)
        (base / 'artifacts/GalaxCore_58231.zip.manifest.json').unlink()
        runner = base / 'slot/test2/run.sh'
        runner.write_text(runner.read_text().replace(
            'if [ "$mode" = timeout ]; then',
            'if [ "$mode" = success ]; then sleep 5; fi\n'
            'if [ "$mode" = timeout ]; then'))
        repository = base / 'svn-repository'
        subprocess.run(['svnadmin', 'create', str(repository)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(['svn', 'import', '--non-interactive', '-m', 'fixture',
                        str(base / 'slot'), repository.as_uri()], check=True,
                       stdout=subprocess.DEVNULL)
        templates = base / 'templates'
        templates.mkdir()
        (templates / 'route.json').write_text(json.dumps(
            {'flow_config': {'route_design': 1, 'original': 1}}))
        port = free_port()
        control_port = free_port()
        common = {
            'network.server_ip': '127.0.0.1', 'server.port': str(port),
            'server.io_threads': '2', 'server.worker_threads': '2',
            'smoke.control_port': str(control_port), 'network.timeout_ms': '2000',
            'worker.svn_url': repository.as_uri(),
            'worker.artifact_root': str(base / 'artifacts'),
            'worker.checkout_parallelism': '1',
            'worker.reconnect_seconds': '1',
            'worker.terminate_grace_seconds': '1',
            'smoke.revision': '58231', 'smoke.connect_wait_seconds': '60',
            'smoke.wait_seconds': '120',
        }
        server_settings = dict(common)
        server_settings.update({
            'server.bind_ip': '127.0.0.1',
            'log.level': 'ERROR', 'log.dir': str(base / 'logs'),
            'server.work_root': str(base / 'slot/test2'),
            'server.artifact_root': str(base / 'artifacts'),
            'server.templates_dir': str(templates),
            'tools.server_binary': str(binary),
            'smoke.case': 'cases/success/run.tcl',
            'smoke.flow': 'route', 'smoke.target': 'fixture',
            'smoke.case_timeout_seconds': '30',
            'smoke.report_dir': str(base / 'server-reports'),
        })
        server_config = base / 'server.conf'
        write_config(server_config, server_settings)
        worker_configs = []
        for machine in ('novarch3', 'novarch4'):
            settings = dict(common)
            settings.update({'worker.slots': '1',
                'worker.slots_root': str(base / machine / 'slots'),
                'worker.output_root': str(base / machine / 'output'),
                'smoke.report_dir': str(base / machine / 'reports')})
            config = base / (machine + '.conf')
            write_config(config, settings)
            worker_configs.append(config)
        processes = []
        try:
            def start(config, args, log):
                env = os.environ.copy()
                env['FORGESCHED_CONFIG'] = str(config)
                if config in worker_configs:
                    env['FORGESCHED_MACHINE_NAME'] = config.stem
                with log.open('w') as stream:
                    process = subprocess.Popen([sys.executable, '-B', '-u',
                        str(root / 'tools/intranet_p10_p14.py')] + args,
                        cwd=str(root), env=env, stdout=stream,
                        stderr=subprocess.STDOUT, start_new_session=True)
                processes.append(process)
                return process
            controller_log = base / 'controller.log'
            controller = start(server_config,
                               ['--role', 'server', '--scale-host', 'novarch4'],
                               controller_log)
            token = None
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                text = controller_log.read_text()
                match = re.search(r'--token ([0-9a-f]{64})', text)
                if match:
                    token = match.group(1)
                    break
                if controller.poll() is not None:
                    raise AssertionError('controller exited early: ' + text)
                time.sleep(.1)
            assert token, controller_log.read_text()
            workers = []
            for config in worker_configs:
                workers.append(start(config, ['--role', 'worker', '--token', token],
                                     base / (config.stem + '.log')))
            codes = [controller.wait(timeout=240)] + [p.wait(timeout=30) for p in workers]
            if codes != [0, 0, 0]:
                raise AssertionError('codes={} controller={} novarch3={} novarch4={}'
                    .format(codes, controller_log.read_text()[-5000:],
                        (base / 'novarch3.log').read_text()[-3000:],
                        (base / 'novarch4.log').read_text()[-3000:]))
            reports = list((base / 'server-reports').glob('p10-p14-*/server-report.json'))
            assert len(reports) == 1
            data = json.loads(reports[0].read_text())
            assert data['verdict'] == 'PASS', data
            assert set(data['stages']) == {'P10', 'P11', 'P12', 'P13', 'P14'}
            assert len(data['stages']['P13']['worker_ids']) == 6
        finally:
            for process in processes:
                stop_group(process)


if __name__ == '__main__':
    main()
