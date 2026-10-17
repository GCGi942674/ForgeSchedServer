"""GUI connection arguments work without deployment files (Python 3.6+)."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='gcrp-gui-cli-') as folder:
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', GCRP_CONFIG='/missing/server.conf')

    def run(arguments):
        return subprocess.run([binary] + arguments, cwd=folder, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=10)

    for arguments in ([], ['--ip', '127.0.0.1'], ['--port', '8080'],
                      ['--ip', 'bad', '--port', '8080'],
                      ['--ip', '127.0.0.1', '--port', '0'],
                      ['--ip', '127.0.0.1', '--port', '65536'],
                      ['--ip', '127.0.0.1', '--port', '8.0'],
                      ['--config', 'server.conf']):
        result = run(arguments)
        assert result.returncode != 0, (arguments, result.stdout)
    assert run(['--help']).returncode == 0

    # Only the command-line endpoint is usable; no config exists in cwd and
    # GCRP_CONFIG deliberately points to an absent file.
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(3)
        listener.settimeout(10)
        errors = []

        def receive(connection, size):
            data = b''
            while len(data) < size:
                part = connection.recv(size - len(data))
                if not part:
                    raise ValueError('truncated request')
                data += part
            return data

        def serve():
            try:
                for expected in ('query_capabilities', 'query_workers', 'query_tasks'):
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        size = struct.unpack('!I', receive(connection, 4))[0]
                        assert 0 < size <= 1048576
                        request = json.loads(receive(connection, size).decode('utf-8'))
                        assert request['type'] == expected, request
                        body = json.dumps({'version': 1, 'type': 'response',
                            'request_id': request['request_id'],
                            'data': {'code': 0, 'result': {}}}).encode('utf-8')
                        connection.sendall(struct.pack('!I', len(body)) + body)
            except Exception as error:
                errors.append(str(error))

        thread = threading.Thread(target=serve)
        thread.start()
        result = run(['--ip', '127.0.0.1', '--port', str(listener.getsockname()[1]), '--probe'])
        thread.join(12)
        assert not thread.is_alive() and not errors, errors
        assert result.returncode == 0 and 'QT_SERVER_PROBE PASS' in result.stdout, result.stdout
print('QT_GUI_CLI PASS')
