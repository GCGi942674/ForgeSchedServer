#!/usr/bin/env python3
"""Export a bounded Server-side testcase/template/archive snapshot (Python 3.6)."""
import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'worker'))
from runtime_config import load_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', help='override gui.catalog_file')
    args = parser.parse_args()
    config = load_config()
    for key in ('server.work_root', 'server.templates_dir', 'server.artifact_root'):
        if not config.get(key):
            raise ValueError('configure ' + key)
    root = Path(config['server.work_root']).expanduser().resolve(strict=True)
    if not root.is_dir() or not Path(config['server.templates_dir']).is_dir() or not Path(config['server.artifact_root']).is_dir():
        raise ValueError('work_root, templates_dir and artifact_root must be directories')
    destination = args.output or config.get('gui.catalog_file')
    if not destination:
        raise ValueError('configure gui.catalog_file or pass --output')
    paths = []
    for directory, dirs, files in os.walk(str(root), followlinks=False):
        dirs[:] = sorted(d for d in dirs if not d.startswith('.') and
                         not (Path(directory) == root and d == 'vivado_runner') and
                         not (Path(directory) / d).is_symlink())
        case = Path(directory) / 'run.tcl'
        if Path(directory) != root and 'run.tcl' in files and case.is_file() and not case.is_symlink():
            paths.append(case.relative_to(root).as_posix())
            if len(paths) > 50000:
                raise ValueError('more than 50000 cases; narrow server.work_root')
    templates = {}
    for file in sorted(Path(config['server.templates_dir']).glob('*.json')):
        if file.is_symlink():
            continue
        data = json.loads(file.read_text(encoding='utf-8'))
        flow = data.get('flow_config')
        if not isinstance(flow, dict) or not flow:
            raise ValueError('invalid flow_config: ' + str(file))
        templates[file.stem] = {'flow_config': flow}
    versions = []
    for file in Path(config['server.artifact_root']).glob('GalaxCore_*.zip'):
        match = re.fullmatch(r'GalaxCore_([0-9]+)\.zip', file.name)
        if match and file.is_file() and not file.is_symlink():
            versions.append(match.group(1))
    payload = json.dumps({'version': 1, 'cases': sorted(set(paths)),
                          'templates': templates,
                          'revisions': sorted(set(versions), key=int, reverse=True)},
                         ensure_ascii=False, indent=2).encode('utf-8')
    if len(payload) > 16 * 1024 * 1024:
        raise ValueError('catalog exceeds 16 MiB; narrow server.work_root')
    path = Path(destination).expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name('.' + path.name + '.tmp-' + str(os.getpid()))
    try:
        with temporary.open('wb') as stream:
            stream.write(payload)
        os.replace(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()
    print('CATALOG: {} cases, {} templates, {} revisions -> {}'.format(
        len(paths), len(templates), len(versions), path))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print('[GUI-CATALOG] ' + str(error), file=sys.stderr)
        sys.exit(1)
