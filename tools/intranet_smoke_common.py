"""Small Python 3.6-compatible helpers for the intranet smoke launchers."""
from collections import deque
from datetime import datetime
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'worker'))
from runtime_config import load_config  # noqa: E402

TERMINAL = ('SUCCEEDED', 'FAILED', 'TIMEOUT', 'CANCELLED')


def utc_now():
    return datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')


def config_and_root(role="server"):
    configured = os.environ.get('GCRP_CONFIG')
    if configured is not None:
        if not configured:
            raise ValueError('GCRP_CONFIG is empty')
        os.environ['GCRP_CONFIG'] = str(Path(configured).expanduser().resolve())
    os.chdir(str(ROOT))
    return load_config(role)


def path_from(config, key):
    value = config.get(key, '').strip()
    if not value:
        raise ValueError('configure ' + key)
    return Path(value).expanduser().resolve()


def number(config, key, default, minimum, maximum):
    try:
        value = int(config.get(key, str(default)))
    except ValueError:
        raise ValueError('invalid ' + key)
    if value < minimum or value > maximum:
        raise ValueError(key + ' must be {}..{}'.format(minimum, maximum))
    return value


def report_path(config, name):
    directory = Path(config.get('smoke.report_dir') or './intranet-report').expanduser().resolve()
    directory.mkdir(parents=True, exist_ok=True)
    return directory / name


def write_report(path, report):
    data = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + '\n'
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=str(path.parent))
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, str(path))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def tail(path, limit=20):
    try:
        with path.open(encoding='utf-8', errors='replace') as stream:
            return list(deque((line.rstrip()[:500] for line in stream if line.strip()), maxlen=limit))
    except OSError:
        return []


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError('server closed response')
        data.extend(chunk)
    return bytes(data)


def request(config, kind, data):
    host = config.get('network.server_ip', '')
    port = number(config, 'server.port', 0, 1, 65535)
    timeout = number(config, 'network.timeout_ms', 5000, 100, 60000) / 1000.0
    if not host:
        raise ValueError('configure network.server_ip')
    body = json.dumps({'version': 1, 'type': kind, 'request_id': 1, 'data': data},
                      separators=(',', ':')).encode('utf-8')
    if len(body) > 1024 * 1024:
        raise ValueError('request exceeds frame limit')
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(struct.pack('!I', len(body)) + body)
        size = struct.unpack('!I', recv_exact(sock, 4))[0]
        if size < 1 or size > 1024 * 1024:
            raise ValueError('invalid response frame')
        message = json.loads(recv_exact(sock, size).decode('utf-8'))
    if (not isinstance(message, dict) or message.get('type') != 'response' or
            message.get('request_id') != 1 or not isinstance(message.get('data'), dict)):
        raise ValueError('invalid GCRP response')
    result = message['data']
    if result.get('code') != 0:
        raise RuntimeError('{}: {}'.format(kind, result.get('message')))
    return result.get('result')
