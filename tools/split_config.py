#!/usr/bin/env python3
"""Split a legacy deployment config without changing any configured paths (Python 3.6)."""
import argparse
import os
from pathlib import Path


def split(source, destination):
    source, destination = Path(source), Path(destination)
    rows = []
    for number, line in enumerate(source.read_text(encoding='utf-8').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if '=' not in line or not line.split('=', 1)[0].strip():
            raise ValueError('malformed config line {}'.format(number))
        key, value = (part.strip() for part in line.split('=', 1))
        rows.append((key, value))
    targets = [destination / 'server.conf', destination / 'worker.conf']
    if any(path.exists() for path in targets):
        raise ValueError('refusing to overwrite existing server.conf or worker.conf')
    destination.mkdir(parents=True, exist_ok=True)
    # Tools/smoke and connection settings are needed independently on each host.
    common = ('network.', 'log.retention_days', 'tools.', 'smoke.')
    server = [(k, v) for k, v in rows if not k.startswith('worker.')]
    worker = [(k, v) for k, v in rows
              if k.startswith(('worker.',) + common) or k == 'server.port']
    created = []
    try:
        for path, values in zip(targets, (server, worker)):
            # Exclusive creation prevents concurrent invocations overwriting deployment data.
            with path.open('x', encoding='utf-8') as stream:
                created.append(path)
                stream.write('# Split from legacy config. Existing paths intentionally preserved.\n')
                stream.write('# Relative paths retain the legacy working-directory semantics.\n')
                for key, value in values:
                    stream.write('{}={}\n'.format(key, value))
                stream.flush()
                os.fsync(stream.fileno())
    except BaseException:
        for path in created:
            path.unlink()
        raise
    return targets


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'config/gcrp.conf')
    parser.add_argument('--destination', type=Path, default=root / 'config')
    args = parser.parse_args()
    for path in split(args.source, args.destination):
        print('CREATED: ' + str(path))
    print('Legacy config and all DB, journal, log and SVN paths are unchanged.')


if __name__ == '__main__':
    main()
