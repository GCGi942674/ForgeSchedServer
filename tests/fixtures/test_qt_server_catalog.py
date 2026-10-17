"""Exercise actual Qt widgets against the actual C++ Server, without a local catalog."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from test_sqlite_worker import wait_server, stop, request

with tempfile.TemporaryDirectory(prefix='gcrp-qt-server-catalog-') as temporary:
    root = Path(temporary)
    cases, templates, archives = root / 'test2', root / 'templates', root / 'zip'
    cases.mkdir()
    templates.mkdir()
    archives.mkdir()
    for index in range(405):
        case = cases / 'device' / ('case' + str(index))
        case.mkdir(parents=True)
        (case / 'run.tcl').write_text('# fixture')
    (templates / 'route.json').write_text(json.dumps({'flow_config': {'route_design': 1, 'bit_cmp': 0}}))
    for index in range(1, 66):
        (archives / ('GalaxCore_' + str(index) + '.zip')).write_bytes(b'fixture')
    (root / 'protected').write_text('1\n')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    config = root / 'config.conf'
    config.write_text('server.bind_ip=127.0.0.1\nserver.port={}\nnetwork.server_ip=127.0.0.1\n'
        'server.io_threads=1\nserver.worker_threads=2\nlog.level=ERROR\nlog.dir={}\n'
        'database.path={}\nserver.work_root={}\nserver.templates_dir={}\nserver.artifact_root={}\n'
        'server.protect_versions_file={}\n'.format(port, root / 'logs', root / 'state.db', cases, templates, archives, root / 'protected'))
    env = dict(os.environ, GCRP_CONFIG=str(config), QT_QPA_PLATFORM='offscreen')
    server = None
    with (root / 'server.log').open('w+') as log:
        try:
            server = subprocess.Popen([str(Path(sys.argv[1]).resolve())], cwd=str(root), env=env, stdout=log, stderr=subprocess.STDOUT)
            wait_server(port, server)
            result = subprocess.run([str(Path(sys.argv[2]).resolve()), '--ip', '127.0.0.1', '--port', str(port), '--catalog-probe'],
                cwd=str(root), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=35, universal_newlines=True)
            print(result.stdout)
            assert result.returncode == 0, result.stdout
            assert 'cases=405 revisions=51 templates=1 flow_options=19' in result.stdout, result.stdout
            # More than one protocol page: test actual Server list DTO -> Qt
            # grouped task model, rather than validating demo data alone.
            for index in range(65):
                response = request(port, 'submit_task', {'task_type': 'REGRESSION',
                    'target': 'device', 'revision': '65', 'payload': {'spec_version': 2,
                    'case': 'device/case{}/run.tcl'.format(index), 'flow': 'route',
                    'flow_config': {'route_design': 1}, 'timeout_seconds': 60,
                    'context': {'batch_id': 'qt-list-fixture', 'name': 'Qt snapshot',
                                'source': 'manual', 'template': 'route'}}})
                assert response['code'] == 0, response
            result = subprocess.run([str(Path(sys.argv[2]).resolve()), '--ip', '127.0.0.1', '--port', str(port), '--snapshot-probe'],
                cwd=str(root), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=35, universal_newlines=True)
            print(result.stdout)
            assert result.returncode == 0, result.stdout
            assert 'tasks=65 batches=1' in result.stdout, result.stdout
        finally:
            stop(server)
print('QT_SERVER_CATALOG PASS')
